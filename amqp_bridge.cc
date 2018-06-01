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
#include "http_parser.h"
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

#include <cctype>
#include <deque>

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace Amqp {

typedef Logger::Loggable<Logger::Id::filter> Loggable;
const char SERVER_NAME[]{"amqp_server"};
const char CLIENT_NAME[]{"amqp_client"};

const std::string FILTER_NAME_PREFIX = "envoy.filters.network.";
const std::string SP = " ";
const std::string COLON_SP = ": ";
const std::string CRLF = "\r\n";
const std::string HTTP_VERSION = "HTTP/1.1";
const std::string POST = "POST";
const std::string ANONYMOUS_RELAY="ANONYMOUS-RELAY";
const Http::LowerCaseString HOST{"host"};
const Http::LowerCaseString CONTENT_LENGTH{"content-length"};
const Http::LowerCaseString CONTENT_TYPE{"content-type"};
const Http::LowerCaseString CONTENT_ENCODING{"content-encoding"};
const Http::LowerCaseString TEXT_PLAIN_UTF8{"text/plain; charset=utf-8"};
const Http::LowerCaseString APPLICATION_OCTET_STREAM{
    "application/octet-stream"};

bool contentHeader(const Http::LowerCaseString &name) {
  return name == CONTENT_LENGTH || name == CONTENT_TYPE ||
         name == CONTENT_ENCODING;
}

bool reservedHeader(const Http::LowerCaseString &name) {
  return contentHeader(name) || name == HOST;
}

inline uint64_t min(uint64_t a, uint64_t b) { return (a < b) ? a : b; }

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
      throw EnvoyException("invalid URI: " + uri_);
    }
  }
  std::string host() const { return field(UF_HOST); }
};

// Convert AMQP messages to HTTP in a Buffer::Instance
class HttpEncoder {
public:
  HttpEncoder(Buffer::Instance &http) : http_(http) {}

  // @throw EnvoyException
  void request(const proton::delivery &d, const proton::message &m) {
    std::string method = m.subject().empty() ? POST : m.subject();
    std::string uri =
        m.address().empty() ? d.receiver().target().address() : m.address();
    std::string host = Uri(uri).host();
    if (host.empty())
      host = d.connection().virtual_host(); // Fallback to AMQP vhost

    // Can't have any exceptions once we start writing to HTTP buffer, it may
    // already have previous HTTP data in it.
    request_line(method, uri);
    header(HOST, host); // Required by HTTP/1.1
    message(m);
  }

  void response(const proton::message &m) {
    response_line(m.subject());
    message(m);
  }

private:
  void request_line(const std::string &method, const std::string &uri) {
    for (const auto &s : {method, SP, uri, SP, HTTP_VERSION, CRLF})
      http_.add(s);
  }

  void response_line(const std::string &s) {
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
    if (!reservedHeader(name) && kv.second.type() == proton::STRING) {
      header(name, proton::get<std::string>(kv.second));
    }
  }

  // Common headers and body
  void message(const proton::message &m) {
    std::string default_type;

    // Figure out what kind of body we have
    switch (m.body().type()) {

    case proton::NULL_TYPE:
      break; // No body
    case proton::STRING:
      proton::get<std::string>(m.body(), content_);
      default_type = TEXT_PLAIN_UTF8.get();
      break;

    case proton::BINARY:
      proton::coerce<std::string>(m.body(), content_);
      if (!m.inferred()) { // An AMQP binary value, set the content type
        default_type = APPLICATION_OCTET_STREAM.get();
      } // Otherwise this is a data section, respect m.content_type even if
        // empty
      break;

    default:
      throw EnvoyException("invalid AMQP body type: " +
                           proton::type_name(m.body().type()));
      break;
    }

    std::map<std::string, proton::scalar> properties;
    proton::get(m.properties().value(), properties);
    for (const auto &kv : properties) {
      header(kv);
    }

    if (m.body().empty()) {
      http_.add(CRLF);
    } else {
      header(CONTENT_TYPE,
             (m.content_type().empty() ? default_type : m.content_type()));
      if (m.content_encoding().empty()) {
        header(CONTENT_LENGTH, std::to_string(content_.size()));
      } else {
        header(CONTENT_ENCODING, m.content_encoding());
      }
      http_.add(CRLF); // Header/body separator
      http_.add(content_);
    }
  }

  Buffer::Instance &http_;
  std::string content_; // Allow std::string to re-use memory
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

/// Base class bridges between AMQP and HTTP on a single connection regardless
/// of whether the connection is incoming/outgoing or direction of request and
/// response
///
class AmqpBridge : public proton::messaging_handler,
                   public Network::Filter,
                   protected Loggable {
public:
  AmqpBridge() { connection_opts_.handler(*this).container_id(container_id_); }

  // == Envoy Network::Filter callbacks

  void
  initializeReadFilterCallbacks(Network::ReadFilterCallbacks &cb) override {
    read_callbacks_ = &cb;
  }

  Network::FilterStatus onNewConnection() override {
    conn().enableHalfClose(true);
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
    http_in_.move(data);
    process(false, end_stream);
    data.move(amqp_out_); // Replace with AMQP data
    return Network::FilterStatus::Continue;
  }

  // == proton::messaging_handler callbacks

  void on_sender_open(proton::sender &s) override {
    if (!s.active()) { // Incoming link
      proton::sender_options opts;
      if (s.source().dynamic()) {
        opts.source(proton::source_options().address(
            s.connection().container_id() + "/" + s.name()));
      }
      s.open(opts);
    }
    senders_[s.source().address()] = s;
  }

  void on_sender_close(proton::sender &s) override {
    senders_.erase(s.source().address());
  };

protected:
  const std::string container_id_{"envoy-" + proton::uuid::random().str()};
  proton::connection_options connection_opts_;
  proton::io::connection_driver driver_;
  Buffer::OwnedImpl amqp_in_, amqp_out_, http_in_, http_out_, empty_;
  std::unordered_map<std::string, proton::sender> senders_;

  virtual void processHttp(bool end_http) PURE;

  Network::Connection &conn() { return read_callbacks_->connection(); }

  void processHttpNoThrow(bool end_http) {
    try {
      processHttp(end_http);
    } catch (const std::exception &e) {
      driver_.connection().close(
          proton::error_condition("amqp:internal-error", e.what()));
      driver_.write_close();
    }
    if (end_http) {
      driver_.connection().close();
      driver_.write_close();
    }
  }

  void process(bool end_amqp, bool end_http) {
    do { // AMQP event loop
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
      if (end_amqp) {
        driver_.read_close();
      }
      processHttpNoThrow(end_http);
    } while (driver_.has_events() && driver_.dispatch());
  }

private:
  Network::ReadFilterCallbacks *read_callbacks_{nullptr};

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
  AmqpServer() { driver_.accept(connection_opts_); }

  // == proton::messaging_handler overrides

  // AMQP request message
  void on_message(proton::delivery &d, proton::message &m) override {
    try {
      auto i = senders_.find(m.reply_to());
      if (i == senders_.end())
        throw EnvoyException("unknown reply_to");
      ENVOY_CONN_LOG(debug, "[amqp_server] received request: {}", conn(), m);
      requests_.push_back(Request(d, m, i->second));
      HttpEncoder(http_out_).request(d, m); // Forward HTTP request
    } catch (const std::exception &e) {
      ENVOY_CONN_LOG(error, "[amqp_server] bad request: {}: {}", conn(),
                     e.what(), m);
      proton::message m(e.what());
      m.subject("400 Bad Request");
      sendResponse(m); // TODO aconway 2018-05-03: should be a response also
    }
  }

protected:
  // TODO aconway 2018-06-04: make outgoing links? Good for dispatch, rude for
  // normal clients.

  // HTTP response to AMQP response
  void processHttp(bool end_http) {
    while (response_.parse(http_in_, end_http)) {
      if (requests_.empty()) {
        ENVOY_CONN_LOG(error, "[amqp_server] unexpected response: {}", conn(),
                       response_.message());
      } else {
        int code = response_.code();
        std::string status = response_.status();
        int type = code / 100;
        switch (type) {
        case 1: // Informational
        case 3: // Redirect
          ENVOY_CONN_LOG(debug, "[amqp_server] ignoring response: '{} {}'",
                         conn(), code, status);
          continue;
        }
        auto res = response_.message();
        sendResponse(res);
      }
    }
  }

  void sendResponse(proton::message &res) {
    auto req = std::move(requests_.front());
    requests_.pop_front();
    res.to(req.message.reply_to());
    auto correlation = req.message.correlation_id();
    if (!correlation.empty())
      res.correlation_id(correlation);
    ENVOY_CONN_LOG(debug, "[amqp_server] sending response: {}", conn(), res);
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

  std::deque<Request> requests_;
  AmqpResponseBuilder response_;
};

/// Filter for incoming connections from AMQP request/response clients.
class AmqpClient : public AmqpBridge {
public:
  AmqpClient() {
    driver_.connect(connection_opts_);
    proton::receiver_options ro;
    ro.source(proton::source_options().dynamic(true));
    receiver_ = driver_.connection().open_receiver("", ro);
  }

  // == proton::messaging_handler overrides

  void on_receiver_open(proton::receiver &r) override {
    if (r == receiver_) {
      receiver_ready_ = true;
      processHttpNoThrow(
          false); // Process any backlog while waiting for receiver
    }
  }

  // AMQP response message
  void on_message(proton::delivery &, proton::message &m) override {
    sendResponse(m);
  }

  // AMQP failed settlement is converted a response message
  void on_tracker_settle(proton::tracker &t) override {
    auto i = trackers_.find(t);
    if (i != trackers_.end()) {
      if (t.state() != proton::transfer::ACCEPTED) {
        proton::message m(proton::to_string(t.state()));
        m.correlation_id(i->second);
        m.subject("502 Bad Gateway - " + proton::to_string(t.state()));
        sendResponse(m);
      }
      trackers_.erase(i);
    }
  }

protected:
  void sendResponse(proton::message &m) {
    if (proton::type_id_is_integral(m.correlation_id().type())) {
      auto i = requests_.find(proton::coerce<uint64_t>(m.correlation_id()));
      if (i != requests_.end()) {
        i->second.respond(std::move(m));
        sendHttpResponses();
        return;
      }
      ENVOY_CONN_LOG(error,
                     "client received response with bad correlation-id: {}",
                     conn(), m);
    }
  }

  void processHttp(bool end_http) {
    if (receiver_ready_) { // Wait till we have a reply_to address
      while (request_.parse(http_in_, end_http)) {
        auto cid = next_id_++;
        proton::message &m = request_.message();
        requests_[cid]; // Create a slot for the request
        m.correlation_id(cid);
        m.reply_to(receiver_.source().address());
        auto s = senders_[m.address()];
        if (!s) {
          s = senders_[m.address()] =
              driver_.connection().open_sender(m.address());
        }
        ENVOY_CONN_LOG(debug, "[amqp_client] sending request: {}", conn(), m);
        auto tracker = s.send(m);
        trackers_[tracker] = cid;
      }
    }
  }

private:
  struct Request {
    proton::message response;
    bool ready{false};

    void respond(proton::message &&m) {
      response = m;
      ready = true;
    }
  };

  AmqpRequestBuilder request_;
  proton::receiver receiver_;
  bool receiver_ready_{false};
  uint64_t next_id_{0};
  std::map<uint64_t, Request> requests_;
  std::map<proton::tracker, uint64_t> trackers_;

  void sendHttpResponses() {
    // Send responses in request order
    auto i = requests_.begin();
    for (; i != requests_.end() && i->second.ready; ++i) {
      Request &r = i->second;
      ENVOY_CONN_LOG(debug, "[amqp_client] sending response: {}", conn(),
                     r.response);
      HttpEncoder(http_out_).response(i->second.response);
    }
    requests_.erase(requests_.begin(), i);
  }
};

using namespace Server::Configuration;

template <class Filter, const char *Name>
class ConfigFactory : public NamedNetworkFilterConfigFactory {
public:
  Network::FilterFactoryCb
  createFilterFactoryFromProto(const Protobuf::Message &,
                               FactoryContext &fc) override {
    return [this, &fc](Network::FilterManager &filter_manager) -> void {
      filter_manager.addFilter(std::make_shared<Filter>());
    };
  }

  ProtobufTypes::MessagePtr createEmptyConfigProto() override {
    return ProtobufTypes::MessagePtr{new Envoy::ProtobufWkt::Empty()};
  }

  std::string name() override { return FILTER_NAME_PREFIX + Name; }

  Network::FilterFactoryCb createFilterFactory(const Json::Object &,
                                               FactoryContext &) override {
    NOT_IMPLEMENTED;
  }

  typedef Registry::RegisterFactory<ConfigFactory<Filter, Name>,
                                    NamedNetworkFilterConfigFactory>
      Register;
};

static ConfigFactory<AmqpServer, SERVER_NAME>::Register server_registerd_;
static ConfigFactory<AmqpClient, CLIENT_NAME>::Register client_registerd_;

} // namespace Amqp
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
