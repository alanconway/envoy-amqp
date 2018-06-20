/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include "common/buffer/buffer_impl.h"
#include "common/common/logger.h"
#include "envoy/buffer/buffer.h"
#include "envoy/http/header_map.h"
#include "envoy/network/connection.h"
#include "envoy/network/filter.h"
#include "envoy/registry/registry.h"
#include "envoy/server/filter_config.h"
#include "extensions/filters/network/common/factory_base.h"
// TODO aconway 2018-06-11: move to envoy config hierarchy.
#include "api/v2/amqp_bridge.pb.validate.h"

#include "proton/codec/map.hpp"
#include "proton/connection.hpp"
#include "proton/delivery.hpp"
#include "proton/io/connection_driver.hpp"
#include "proton/message.hpp"
#include "proton/message_id.hpp"
#include "proton/messaging_handler.hpp"
#include "proton/receiver.hpp"
#include "proton/receiver_options.hpp"
#include "proton/sender.hpp"
#include "proton/sender_options.hpp"
#include "proton/source.hpp"
#include "proton/source_options.hpp"
#include "proton/target.hpp"
#include "proton/target_options.hpp"

#include "http_parser.h"

#include <cctype>
#include <deque>
#include <iomanip>
#include <sstream>

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace Amqp {

namespace {

// typedef verbose proton::*_options classes
typedef proton::connection_options ConOpts;
typedef proton::sender_options SndOpt;
typedef proton::receiver_options RcvOpt;
typedef proton::target_options TgtOpt;
typedef proton::source_options SrcOpt;

typedef Logger::Loggable<Logger::Id::filter> Loggable;

const std::string FILTER_PREFIX = "envoy.filters.network.";

const std::string SP = " ";
const std::string COLON_SP = ": ";
const std::string CRLF = "\r\n";
const std::string HTTP_VERSION = "HTTP/1.1";
const std::string POST = "POST";
const std::string BAD_REQUEST = "400 Bad Request"; // Invalid request
const std::string BAD_GATEWAY = "502 Bad Gateway"; // Invalid response
const std::string SERVICE_UNAVAILABLE = "503 Service Unavailable";

const std::string AMQP_NOT_ALLOWED = "amqp:not-allowed";

const Http::LowerCaseString HOST{"host"};
const Http::LowerCaseString CONTENT_LENGTH{"content-length"};
const Http::LowerCaseString CONTENT_TYPE{"content-type"};
const Http::LowerCaseString CONTENT_ENCODING{"content-encoding"};
const Http::LowerCaseString TRANSFER_ENCODING{"transfer-encoding"};
const Http::LowerCaseString TEXT_PLAIN_UTF8{"text/plain; charset=utf-8"};
const Http::LowerCaseString APPLICATION_OCTET_STREAM{
    "application/octet-stream"};

const proton::symbol ANONYMOUS_RELAY = "ANONYMOUS-RELAY";
const std::vector<proton::symbol> ANONYMOUS_RELAY_CAPABILITIES{ANONYMOUS_RELAY};

// These headers are not copied to/from application-properties:
// They are set as/computed from message-properties and body size.
bool contentHeader(const Http::LowerCaseString &name) {
  return name == CONTENT_LENGTH || name == CONTENT_TYPE ||
         name == CONTENT_ENCODING || name == TRANSFER_ENCODING;
}

inline uint64_t min(uint64_t a, uint64_t b) { return (a < b) ? a : b; }

// String summary of message for log output
std::string log(const proton::message &m) {
  // TODO aconway 2018-06-20: not very efficient, use LOG and fmt:: properly
  std::ostringstream o;
  o << m; // Limit size of long message bodies in logs
  return o.str().substr(0, 512);
}

inline bool is_status_line(const std::string &s) {
  return (s.size() >= 3 && '1' <= s[0] && s[0] <= '5' && std::isdigit(s[1]) &&
          std::isdigit(s[2]) && (s.size() == 3 || s[3] == ' '));
}

class Uri {
  struct http_parser_url parser_;
  std::string uri_;
  std::string field(int fno) const {
    return (parser_.field_set & (1 << fno))
               ? uri_.substr(parser_.field_data[fno].off,
                             parser_.field_data[fno].len)
               : "";
  }

public:
  // @throw EnvoyException
  Uri(const std::string &uri, bool is_connect = false) : uri_(uri) {
    http_parser_url_init(&parser_);
    if (http_parser_parse_url(uri_.data(), uri_.size(), is_connect, &parser_)) {
      throw EnvoyException("invalid URI \"" + uri_ + "\"");
    }
  }
  std::string host() const { return field(UF_HOST); }
};

// Convert AMQP messages to HTTP in a Buffer::Instance
class HttpEncoder {
public:
  // @throw EnvoyException
  Buffer::Instance &request(const proton::delivery &d,
                            const proton::message &m) {
    std::string method = m.subject().empty() ? POST : m.subject();
    std::string uri = m.to().empty() ? d.receiver().target().address() : m.to();
    std::string host = Uri(uri).host();
    if (host.empty())
      host = d.connection().virtual_host(); // Fallback to AMQP vhost

    request_line(method, uri);
    header(HOST, host); // Required by HTTP/1.1
    message(m);
    return http_;
  }

  Buffer::Instance &response(const proton::message &m) {
    response_line(m.subject());
    message(m);
    return http_;
  }

private:
  void request_line(const std::string &method, const std::string &uri) {
    http_.drain(http_.length());
    for (const auto &s : {method, SP, uri, SP, HTTP_VERSION, CRLF})
      http_.add(s);
  }

  void response_line(const std::string &s) {
    http_.drain(http_.length());
    std::string rl =
        is_status_line(s) ? s : (s.empty() ? "200 OK" : "200 " + s);
    for (const auto &s : {HTTP_VERSION, SP, rl, CRLF}) {
      http_.add(s);
    }
  }

  void header(const Http::LowerCaseString &name, const std::string &value) {
    for (const auto &s : {name.get(), COLON_SP, value, CRLF}) {
      http_.add(s);
    }
  }

  void header(const std::pair<std::string, proton::scalar> &kv) {
    Http::LowerCaseString name(kv.first);
    if (!contentHeader(name) && kv.second.type() == proton::STRING) {
      header(name, proton::get<std::string>(kv.second));
    }
  }

  // TODO aconway 2018-06-20: there is a LOT of copying in message
  // encoding/decoding

  // Common headers and body
  void message(const proton::message &m) {
    // Do content-related headers first
    content_type_ = m.content_type();
    if (m.body().empty()) {
      content_.clear();
    } else { // Not empty
      switch (m.body().type()) {
      case proton::STRING:
        if (content_type_.empty()) { // AMQP string value
          content_type_ = TEXT_PLAIN_UTF8.get();
        }
        break;
      case proton::BINARY:
        // Set default content-type for an AMQP binary value,
        // but for a binary data section respect m.content_type even if it is
        // empty.
        if (content_type_.empty() && !m.inferred()) {
          content_type_ = APPLICATION_OCTET_STREAM.get();
        }
        break;

      default:
        throw EnvoyException("'" + proton::type_name(m.body().type()) +
                             "' message-body type is not allowed");
      }
      proton::coerce<std::string>(m.body(), content_);
      header(CONTENT_LENGTH, std::to_string(content_.size()));
      if (!content_type_.empty()) {
        header(CONTENT_TYPE, content_type_);
      }
      if (!m.content_encoding().empty()) {
        header(CONTENT_ENCODING, m.content_encoding());
      }
    }
    // Now the remaining properties
    proton::get(m.properties().value(), properties_);
    for (const auto &kv : properties_) {
      header(kv);
    }
    http_.add(CRLF);     // Header/body separator
    http_.add(content_); // body
  }

  // Re-use these variables, they will cache some memory between use and
  // reduce allocations.
  Buffer::OwnedImpl http_;
  std::string content_, content_type_;
  std::map<std::string, proton::scalar> properties_;
};

// Parse HTTP and build an AMQP message.
class AmqpBuilder : public Loggable {
public:
  AmqpBuilder(enum http_parser_type type) {
    http_parser_init(&parser_, type);
    parser_.data = this;
  }

  virtual ~AmqpBuilder(){};

  // Consume HTTP data from buffer.
  // Return true if the message is complete, false if more data is needed.
  // @throw EnvoyException
  bool parse(Buffer::Instance &http, bool end_stream) {
    done_ = false;
    Buffer::RawSlice slice;
    while (http.length() && !done_) {
      http.getRawSlices(&slice, 1);
      size_t len =
          http_parser_execute(&parser_, &settings_,
                              reinterpret_cast<char *>(slice.mem_), slice.len_);
      http.drain(len);
      int err = parser_.http_errno;
      if (err) {
        std::ostringstream msg;
        msg << "HTTP->AMQP parse error: "
            << http_errno_description(static_cast<http_errno>(err));
        throw EnvoyException(msg.str());
      }
    }
    if (!http.length() && end_stream) {
      // Stream end might be end-of-content indicator, tell the parser
      http_parser_execute(&parser_, &settings_, NULL, 0);
    }
    return done_;
  }

  int code() const { return parser_.status_code; }

  const std::string &status() const { return status_; }

protected:
  proton::message message_;
  std::string url_, field_, value_, status_;
  proton::binary body_;
  bool has_content_;
  http_parser parser_;

private:
  static const http_parser_settings settings_;
  bool in_value_, done_;

  // Reset to parse a new message
  void onMessageBegin() {
    // Clear out for new message
    url_.clear();
    body_.clear();
    field_.clear();
    value_.clear();
    status_.clear();
    message_.clear();
    has_content_ = in_value_ = done_ = false;
  }

  void onUrl(const char *data, size_t length) { url_.append(data, length); }

  void onBody(const char *data, size_t length) {
    const uint8_t *begin = reinterpret_cast<const uint8_t *>(data);
    body_.insert(body_.end(), begin, begin + length);
  };

  void onStatus(const char *data, size_t length) {
    status_.append(data, length);
  };

  void onHeadersComplete() {
    if (in_value_) {
      // First check for headers that map to AMQP message properties
      Http::LowerCaseString lfield(field_);
      if (contentHeader(lfield)) {
        has_content_ = true;
        if (lfield == CONTENT_TYPE) {
          message_.content_type(value_);
        } else if (lfield == CONTENT_ENCODING) {
          message_.content_encoding(value_);
        }
      } else {
        // All other headers map to application properties.
        message_.properties().put(std::move(field_), std::move(value_));
      }
      field_.clear();
      value_.clear();
      in_value_ = false;
    }
  };

  void onHeaderField(const char *data, size_t length) {
    onHeadersComplete(); // Complete the previous header if there was one
    field_.append(data, length);
  }

  void onHeaderValue(const char *data, size_t length) {
    in_value_ = true;
    value_.append(data, length);
  }

  void onMessageComplete() {
    done_ = true;
    if (has_content_) {
      message_.inferred(true); // Data section
      message_.body(std::move(body_));
    }
  }
};

class AmqpRequestBuilder : public AmqpBuilder {
public:
  AmqpRequestBuilder() : AmqpBuilder(HTTP_REQUEST) {}

  proton::message &message() {
    message_.address(url_);
    message_.subject(http_method_str(static_cast<http_method>(parser_.method)));
    return message_;
  }
};

class AmqpResponseBuilder : public AmqpBuilder {
public:
  AmqpResponseBuilder() : AmqpBuilder(HTTP_RESPONSE) {}
  proton::message &message() {
    message_.subject(std::to_string(parser_.status_code) + " " + status_);
    return message_;
  }
};

// http callback wrapper templates

template <void (AmqpBuilder::*f)()> int mb_cb(http_parser *parser) {
  (static_cast<AmqpBuilder *>(parser->data)->*f)();
  return 0; // f() will throw on error
}

template <void (AmqpBuilder::*f)(const char *, size_t)>
int mb_data_cb(http_parser *parser, const char *data, size_t length) {
  (static_cast<AmqpBuilder *>(parser->data)->*f)(data, length);
  return 0; // f() will throw on error
}

const http_parser_settings AmqpBuilder::settings_ = {
    .on_message_begin = &mb_cb<&AmqpBuilder::onMessageBegin>,
    .on_url = &mb_data_cb<&AmqpBuilder::onUrl>,
    .on_status = &mb_data_cb<&AmqpBuilder::onStatus>,
    .on_header_field = &mb_data_cb<&AmqpBuilder::onHeaderField>,
    .on_header_value = &mb_data_cb<&AmqpBuilder::onHeaderValue>,
    .on_headers_complete = &mb_cb<&AmqpBuilder::onHeadersComplete>,
    .on_body = &mb_data_cb<&AmqpBuilder::onBody>,
    .on_message_complete = &mb_cb<&AmqpBuilder::onMessageComplete>,
    .on_chunk_header = nullptr,
    .on_chunk_complete = nullptr};

void correlate(proton::message &m, const proton::message_id &id) {
  if (!id.empty()) { // Don't set a null correlation-id
    m.correlation_id(id);
  }
}

} // namespace

// Shortcuts for logging with filter name in AmqpBridge subclasses.
#define LOG(LEVEL, FORMAT, ...)                                                \
  ENVOY_LOG(LEVEL, "[{}] " FORMAT, name_, ##__VA_ARGS__)
#define CONN_LOG(LEVEL, FORMAT, ...)                                           \
  ENVOY_CONN_LOG(LEVEL, "[{}] " FORMAT, conn(), name_, ##__VA_ARGS__)

/// Base class bridges between AMQP and HTTP on a single connection regardless
/// of whether the connection is incoming/outgoing or direction of request and
/// response
///
class AmqpBridge : public proton::messaging_handler,
                   public Network::Filter,
                   protected Loggable {
public:
  typedef envoy::config::filter::network::amqp_bridge::v2::AmqpBridge Config;

  AmqpBridge(const std::string &name, const Config &config)
      : name_(name), anonymous_relay_(config.anonymous_relay()),
        named_relay_(config.named_relay()),
        sources_(config.sources().begin(), config.sources().end()),
        targets_(config.targets().begin(), config.targets().end()) {

    auto id = config.id().empty() ? "envoy-" + proton::uuid::random().str()
                                  : config.id();
    connection_opts_.handler(*this).container_id(id).desired_capabilities(
        ANONYMOUS_RELAY_CAPABILITIES);
    receiver_opts_.auto_accept(false); // Options for all receivers
  }

  // == Network::Filter callbacks

  void
  initializeReadFilterCallbacks(Network::ReadFilterCallbacks &cb) override {
    read_callbacks_ = &cb;
    conn().enableHalfClose(true);
  }

  Network::FilterStatus onNewConnection() override {
    return Network::FilterStatus::Continue;
  }

  // Replace AMQP data with HTTP data.
  Network::FilterStatus onData(Buffer::Instance &data,
                               bool end_stream) override {
    amqp_in_.move(data);
    process(end_stream, false);
    data.move(http_out_); // Replace with HTTP data
    if (amqp_out_.length()) {
      forceWrite(); // Make sure this data gets written
    }
    return Network::FilterStatus::Continue;
  }

  // Replace HTTP data with AMQP data
  Network::FilterStatus onWrite(Buffer::Instance &data,
                                bool end_stream) override {
    ASSERT(http_out_.length() == 0);
    http_in_.move(data);
    process(false, end_stream);
    data.move(amqp_out_);            // Replace with AMQP data
    ASSERT(http_out_.length() == 0); // Should not generate HTTP data here.
    return Network::FilterStatus::Continue;
  }

  // == proton::messaging_handler callbacks

  void on_error(const proton::error_condition &e) override {
    CONN_LOG(error, "AMQP error: {}", e);
    driver_.connection().close(e);
  }

  void on_connection_open(proton::connection &c) override {
    c.open(connection_opts_);
    if (anonymous_relay_ && anonymousOffered(c)) {
      CONN_LOG(debug, "peer id \"{}\", anonymous relay", c.container_id());
      relay_ = driver_.connection().open_sender(
          "", SndOpt().target(TgtOpt().anonymous(true)));
    } else if (!named_relay_.empty()) {
      CONN_LOG(debug, "peer id \"{}\", named relay: ", c.container_id(),
               named_relay_);
      relay_ = driver_.connection().open_sender(named_relay_);
    } else {
      CONN_LOG(debug, "peer id \"{}\", no relay", c.container_id());
    }
    for (const auto &s : sources_) {
      driver_.connection().open_receiver(s, receiver_opts_);
    }
    for (const auto &t : targets_) {
      getSender(t, true);
    }
  }

  void on_sender_open(proton::sender &s) override {
    if (!s.active()) { // Incoming
      std::string addr;
      if (s.source().dynamic()) {
        addr = s.connection().container_id() + "/" + s.name();
        CONN_LOG(debug, "incoming sender link from dynamic source \"{}\"",
                 addr);
      } else {
        addr = s.source().address();
        CONN_LOG(debug, "incoming sender link from source \"{}\"", addr);
      }
      auto &ref = senders_[addr];
      if (ref) {
        ref.close(); // TODO aconway 2018-06-27: link-stolen error
      }
      ref = s;
      s.open(SndOpt().source((SrcOpt().address(addr))));
    }
  }

  void on_sender_close(proton::sender &s) override {
    CONN_LOG(debug, "sender link closed: source=\"{}\" target=\"{}\"",
             s.source().address(), s.target().address());
    // Might be recorded by source or target.
    eraseSender(s, s.target().address());
    eraseSender(s, s.source().address());
  };

  void on_receiver_open(proton::receiver &r) override {
    if (!r.active()) { // Incoming
      if (r.target().anonymous()) {
        CONN_LOG(debug, "incoming link to anonymous relay");
      } else {
        CONN_LOG(debug, "incoming link to target \"{}\"", r.target().address());
      }
      r.open(receiver_opts_);
    }
  }

  void on_receiver_close(proton::receiver &r) override {
    CONN_LOG(debug, "receiver link closed: source=\"{}\" target=\"{}\"",
             r.source().address(), r.target().address());
  };

protected:
  const std::string name_;
  proton::connection_options connection_opts_;
  proton::receiver_options receiver_opts_;
  bool anonymous_relay_{false};
  std::string named_relay_;
  std::vector<std::string> sources_;
  std::vector<std::string> targets_;
  proton::io::connection_driver driver_;

  // TODO aconway 2018-06-25: review flow control
  //
  // These buffers are not explicitly bounded, but they are limited by what can
  // be produced from a single read or write event on the underlying Envoy
  // connection. Review whether additional flow control measures are needed.
  //
  Buffer::OwnedImpl amqp_in_, amqp_out_, http_in_, http_out_, empty_;
  HttpEncoder http_encoder_;

  // Read as much of http_in_ as possible (there may be partial data left
  // unread) and generate AMQP events.
  //
  // Client/server implement this differently to parse HTTP requests/responses
  virtual void processHttp(bool end_http) PURE;

  // Fabricate an error response when we don't have a HTTP resonse.
  proton::message makeResponse(const proton::message_id &id,
                               const std::string &subject,
                               const std::string &description) {
    proton::message res;
    correlate(res, id);
    res.subject(subject);
    res.body(description);
    return res;
  }

  Network::Connection &conn() { return read_callbacks_->connection(); }

  proton::sender getSender(const std::string target, bool create) {
    // Try direct sender links first, then relay, then opening a new link if
    // create=true
    auto i = senders_.find(target);
    if (i != senders_.end()) {
      return i->second;
    }
    if (relay_) {
      return relay_;
    }
    if (create) {
      // TODO aconway 2018-06-26: we never close senders
      return senders_[target] = driver_.connection().open_sender(target);
    }
    return proton::sender();
  }

  // Process available HTTP data and AMQP data and AMQP events.
  // @param end_amqp: the AMQP-in stream (connection read stream) has ended
  // @param end_http: the HTTP-in stream (connection write stream) has ended
  void process(bool end_amqp, bool end_http) {
    try {
      processHttp(end_http); // Read http_in_, generate AMQP events
      do {
        processAmqp(); // Read amqp_in_, dispatch events, write amqp_out_
        if (end_http && !http_in_.length()) {
          // Write side locally closed, no more HTTP coming - do polite AMQP
          // protocol close.
          CONN_LOG(debug, "No more HTTP data, closing AMQP connection");
          driver_.connection().close();
        }
        if (end_amqp && !amqp_in_.length()) {
          // Read side remotely closed, inform the driver to run final events.
          driver_.read_close();
        }
        processHttp(end_http); // AMQP processing may unblock HTTP processing
        // close() calls or processHttp() can generate more events.
      } while (driver_.has_events());
    } catch (const std::exception &e) {
      // Disaster, slam the Envoy connection shut, throw away everything.
      CONN_LOG(error, "unexpected error: {}", e.what());
      conn().close(Network::ConnectionCloseType::NoFlush);
    }
  }

  // Read AMQP data from the amqp_in_ buffer, dispatch events,
  // write to the amqp_out_ buffer. Continue till amqp_in_ is empty
  // or driver is finished.
  //
  void processAmqp() {
    bool running = driver_.dispatch();
    while (running && (driver_.write_buffer().size || amqp_in_.length())) {
      auto wbuf = driver_.write_buffer();
      if (wbuf.size) {
        amqp_out_.add(wbuf.data, wbuf.size);
        driver_.write_done(wbuf.size);
      }
      auto rbuf = driver_.read_buffer();
      auto rsize = min(rbuf.size, amqp_in_.length());
      if (rsize) {
        amqp_in_.copyOut(0, rsize, rbuf.data);
        amqp_in_.drain(rsize);
        driver_.read_done(rsize);
      }
      running = driver_.dispatch();
    }
  }

private:
  proton::sender relay_;
  std::unordered_map<std::string, proton::sender> senders_;
  Network::ReadFilterCallbacks *read_callbacks_{nullptr};

  static bool anonymousOffered(proton::connection &c) {
    const auto &cap = c.offered_capabilities();
    return std::find(cap.begin(), cap.end(), ANONYMOUS_RELAY) != cap.end();
  }

  void eraseSender(proton::sender &s, const std::string &a) {
    auto i = senders_.find(a);
    if (i != senders_.end() && i->second == s) {
      senders_.erase(i);
    }
  }

  void forceWrite() {
    // TODO aconway 2018-05-02: hack, need a filter-specific write() call
    // that only gets intercepted by downstream filters. This is a workaround
    // for now - send an empty write to wake up onWrite() so it can notice
    // and write buffered output data.
    conn().write(empty_, false);
  }
};

/// Filter for incoming connections from AMQP request/response clients.
class AmqpServer : public AmqpBridge {
public:
  typedef envoy::config::filter::network::amqp_bridge::v2::AmqpServer Config;

  static const std::string NAME;

  AmqpServer(const Config &config)
      : AmqpBridge(NAME, config.bridge()), auto_link_(config.auto_link()) {
    connection_opts_.offered_capabilities(ANONYMOUS_RELAY_CAPABILITIES);
    driver_.accept(connection_opts_);
  }

  // == proton::messaging_handler overrides

  // AMQP request message
  void on_message(proton::delivery &d, proton::message &m) override {
    CONN_LOG(debug, "received request: {}", log(m));
    proton::sender s = getSender(m.reply_to(), auto_link_);
    if (!s) {
      CONN_LOG(error, "unknown reply_to address: {}", log(m));
      d.reject();
      return;
    }
    try {
      http_out_.move(http_encoder_.request(d, m)); // Forward HTTP request
      requests_.push_back(Request(d, m, s));
    } catch (const std::exception &e) {
      CONN_LOG(error, "cannot encode request: {}", e.what());
      d.reject();
    }
  }

protected:
  // Parse all available HTTP responses, generate AMQP responses
  void processHttp(bool end_http) {
    while (response_.parse(http_in_, end_http)) {
      if (requests_.empty()) {
        CONN_LOG(error, "unexpected response: {}", log(response_.message()));
        throw EnvoyException("unexpected response");
      } else {
        sendResponse(std::move(response_.message()));
      }
    }
    if (end_http) { // Upstream close, inform remaining requests
      for (auto &r : requests_) {
        sendResponse(makeResponse(r.message.correlation_id(),
                                  SERVICE_UNAVAILABLE, "upstream disconnect"));
      }
    }
  }

  void sendResponse(proton::message &&res) {
    auto req = std::move(requests_.front());
    requests_.pop_front();
    res.to(req.message.reply_to());
    correlate(res, req.message.correlation_id());
    CONN_LOG(debug, "sending response: {}", log(res));
    req.delivery.accept(); // Ack delayed till HTTP processing complete.
    // TODO aconway 2018-06-25: queues in excess of credit, flow control
    req.sender.send(res);
  }

private:
  struct Request {
    proton::delivery delivery;
    proton::message message;
    proton::sender sender;
    Request(proton::delivery &d, proton::message &m, proton::sender &s)
        : delivery(d), message(m), sender(s) {}
  };

  bool auto_link_{false};
  std::deque<Request> requests_;
  AmqpResponseBuilder response_;
};

const std::string AmqpServer::NAME = "amqp_server";

/// Filter for incoming connections from AMQP request/response clients.
/// - creates a single unique (dynamic) reply address
/// - correlates and orders AMQP responses messages using integer
/// correlation-ids
/// - sends HTTP responses in request order, even if AMQP responses are out of
/// order
///
class AmqpClient : public AmqpBridge {
public:
  typedef envoy::config::filter::network::amqp_bridge::v2::AmqpClient Config;

  static const std::string NAME;

  AmqpClient(const Config &config) : AmqpBridge(NAME, config.bridge()) {
    driver_.connect(connection_opts_);
    receiver_ = driver_.connection().open_receiver(
        "", RcvOpt(receiver_opts_).source(SrcOpt().dynamic(true)));
  }

  // == proton::messaging_handler overrides

  void on_receiver_open(proton::receiver &r) override {
    if (r == receiver_) {
      CONN_LOG(debug, "dynamic reply_to address: '{}'", r.source().address());
      receiver_ready_ = true;
    }
    AmqpBridge::on_receiver_open(r);
  }

  // AMQP response message
  void on_message(proton::delivery &d, proton::message &m) override {
    recordResponse(std::move(m));
    d.accept();
  }

  // AMQP failed outcome is converted a response message
  void on_tracker_settle(proton::tracker &t) override {
    auto i = trackers_.find(t);
    if (i != trackers_.end()) {
      if (t.state() != proton::transfer::ACCEPTED) {
        auto what = proton::to_string(t.state());
        CONN_LOG(error, "AMQP message {}", what);
        recordResponse(makeResponse(i->second, BAD_GATEWAY, what));
      }
      trackers_.erase(i);
    }
  }

protected:
  // Parse all available HTTP requests, generate AMQP requests
  void processHttp(bool end_http) {
    if (receiver_ready_) { // Wait till we have a reply_to address
      while (request_.parse(http_in_, end_http)) {
        proton::message &m = request_.message();
        auto cid = next_id_++;
        m.correlation_id(cid);
        m.reply_to(receiver_.source().address());
        responses_[cid]; // Create a slot for the response
        proton::sender s = getSender(m.address(), true);
        CONN_LOG(debug, "sending request: {}", log(m));
        // TODO aconway 2018-06-25: queues in excess of credit, flow control
        trackers_[s.send(m)] = cid;
      }
    }
  }

private:
  typedef uint64_t CorrelationId;
  AmqpRequestBuilder request_;
  proton::receiver receiver_;
  bool receiver_ready_{false};
  CorrelationId next_id_{1};
  std::map<CorrelationId, proton::message> responses_;
  std::map<proton::tracker, CorrelationId> trackers_;

  CorrelationId getCorrelation(const proton::message &m) {
    try {
      return proton::coerce<CorrelationId>(m.correlation_id());
    } catch (const std::exception &e) {
      CONN_LOG(error, "invalid correlation-id {}: {}",
               m.correlation_id(), log(m));
      throw EnvoyException("invalid correlation id");
    }
  }

  void recordResponse(proton::message &&m) {
    auto id = getCorrelation(m);
    auto i = responses_.find(id);
    if (i != responses_.end() && i->second.correlation_id().empty()) {
      CONN_LOG(debug, "received response: {}", log(m));
      i->second = m;
      sendHttpResponses();
    } else {
      CONN_LOG(error, "unknown correlation-id {}: {}",
               m.correlation_id(), log(m));
      throw EnvoyException("unknown correlation-id");
    }
  }

  void sendHttpResponses() {
    // Note: correlation IDs are integers so map order is also request order.
    auto i = responses_.begin();
    for (; i != responses_.end() && !i->second.correlation_id().empty(); ++i) {
      proton::message &m = i->second;
      try {
        http_out_.add(http_encoder_.response(m));
      } catch (const std::exception &e) {
        auto s = fmt::format("response encoding error: {}: ", e.what());
        CONN_LOG(error, "{}: {}", s, log(m));
        auto m2 = makeResponse(m.correlation_id(), BAD_GATEWAY, s);
        http_out_.add(http_encoder_.response(m2));
      }
    }
    responses_.erase(responses_.begin(), i);
  }
};

const std::string AmqpClient::NAME = "amqp_client";

template <class Filter>
class Factory : public Common::FactoryBase<typename Filter::Config>,
                protected Loggable {
  typedef typename Filter::Config Config;

public:
  Factory() : Common::FactoryBase<Config>(FILTER_PREFIX + Filter::NAME) {}

  Network::FilterFactoryCb
  createFilterFactory(const Json::Object &,
                      Server::Configuration::FactoryContext &) {
    NOT_IMPLEMENTED;
  }

  typedef Registry::RegisterFactory<
      Factory<Filter>, Server::Configuration::NamedNetworkFilterConfigFactory>
      Register;

private:
  Network::FilterFactoryCb createFilterFactoryFromProtoTyped(
      const Config &config, Server::Configuration::FactoryContext &) override {
    return [=](Network::FilterManager &filter_manager) -> void {
      filter_manager.addFilter(std::make_shared<Filter>(config));
    };
  }
};

static Factory<AmqpClient>::Register client_registered_;
static Factory<AmqpServer>::Register server_registered_;

} // namespace Amqp
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
