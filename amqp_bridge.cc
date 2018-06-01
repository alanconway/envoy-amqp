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
#include "common/common/assert.h"
#include "common/common/logger.h"
#include "envoy/buffer/buffer.h"
#include "envoy/network/connection.h"
#include "envoy/network/filter.h"
#include "envoy/http/header_map.h"
#include "envoy/registry/registry.h"
#include "envoy/server/filter_config.h"
#include "http_parser.h"
#include "proton/codec/vector.hpp"
#include "proton/connection.hpp"
#include "proton/delivery.hpp"
#include "proton/io/connection_driver.hpp"
#include "proton/message.hpp"
#include "proton/messaging_handler.hpp"
#include "proton/message_id.hpp"
#include "proton/receiver.hpp"
#include "proton/receiver_options.hpp"
#include "proton/sender.hpp"
#include "proton/sender_options.hpp"
#include "proton/source.hpp"
#include "proton/source_options.hpp"
#include "proton/target.hpp"
#include "proton/target_options.hpp"

#include <deque>

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace AmqpBridge {

namespace {

inline uint64_t min(uint64_t a, uint64_t b) { return (a < b) ? a : b; }

const std::string NAME = "envoy.filters.network.amqp_bridge";
const std::string SPACE = " ";
const std::string COLON_SPACE = ": ";
const std::string CRLF = "\r\n";
const std::string HTTP_VERSION_CRLF = "HTTP/1.1\r\n";
const std::string POST = "POST";
const std::string HOST { "host" };
const Http::LowerCaseString CONTENT_LENGTH { "content-length" };
const Http::LowerCaseString CONTENT_TYPE { "content-type" };
const Http::LowerCaseString AMQP_CORRELATION_ID { "amqp-correlation-id" };

}

class Uri {
  struct http_parser_url parser_;
  std::string uri_;

 public:
  struct Error : public EnvoyException { using EnvoyException::EnvoyException; };

  Uri(const std::string& uri, bool is_connect=false) : uri_(uri) {
    http_parser_url_init(&parser_);
    if (http_parser_parse_url(uri_.data(), uri_.size(), is_connect, &parser_)) {
      throw Error("invalid uri: " + uri_);
    }
  }

  std::string host() const {
    return (parser_.field_set & (1 << UF_HOST)) ?
      uri_.substr(parser_.field_data[UF_HOST].off, parser_.field_data[UF_HOST].len) : "";
  }
};

// Convert AMQP message to HTTP and append to a Buffer::Instance
class HttpRequestBuilder {
  Buffer::Instance& http_;
  std::string content_;          // Allow std::string to reserve some memory

 public:
  HttpRequestBuilder(Buffer::Instance& http) : http_(http) {}

  void request(const std::string& method, const std::string& uri) {
    http_.add(method);
    http_.add(SPACE);
    http_.add(uri);
    http_.add(SPACE);
    http_.add(HTTP_VERSION_CRLF);
  }

  void header(const std::string& key, const std::string &value) {
    http_.add(key);
    http_.add(COLON_SPACE);
    http_.add(value);
    http_.add(CRLF);
  }

  // @throw Uri::Error on invalid URI
  void message(proton::delivery& d, const proton::message& m) {
    std::string method = m.subject().empty() ? POST: m.subject();
    std::string uri = m.address().empty() ? d.receiver().target().address() : m.address();
    std::string host = Uri(uri).host(); // May throw Uri::Error
    if (host.empty()) host = d.connection().virtual_host(); // Fallback to AMQP vhost

    // Can't have any exceptions once we start writing to HTTP buffer, it may already have
    // previous HTTP data in it.
    request(method, uri);
    header(HOST, host); // Required by HTTP/1.1
    if (!m.content_type().empty()) header(CONTENT_TYPE.get(), m.content_type());
    if (!m.correlation_id().empty()) header(AMQP_CORRELATION_ID.get(), to_string(m.correlation_id()));
    std::vector<std::pair<std::string, proton::scalar> > properties;
    proton::get(m.properties().value(), properties);
    for (auto& kv : properties) {
      auto t =  kv.second.type();
      if (t == proton::STRING || t == proton::SYMBOL) {
        header(kv.first, proton::coerce<std::string>(kv.second));
      } else if (type_id_is_signed_int(t)) {
        header(kv.first, std::to_string(proton::coerce<long long>(kv.second)));
      } else if (type_id_is_unsigned_int(t)) {
        header(kv.first, std::to_string(proton::coerce<unsigned long long>(kv.second)));
      }
    }
    // FIXME aconway 2018-04-23: data segment decoding for basic case + simple conversions.
    // See email thread.
    if (proton::type_id_is_string_like(m.body().type())) {
      proton::coerce<std::string>(m.body(), content_);
      header(CONTENT_LENGTH.get(), std::to_string(content_.size()));
      http_.add(CRLF);             // Header/body separator
      http_.add(content_);
    } else {
      http_.add(CRLF);             // Header/body separator
    }
  }
};

// Parse HTTP and make an appropriate AMQP response to the AMQP request.
// The AMQP response settles the request with a suitable disposition,
// and sends a response message when the request is accepted.
class AmqpResponseBuilder : public Logger::Loggable<Logger::Id::filter>
{
  static const http_parser_settings settings_;
  http_parser parser_;
  std::string url_, body_, field_, value_, status_;
  proton::message response_;  bool in_value_, done_;
  Network::Connection* conn_ {nullptr};

 public:

  AmqpResponseBuilder() {
    http_parser_init(&parser_, HTTP_RESPONSE);
    parser_.data = this;
  }

  // Reset to parse a new message
  void onMessageBegin() {
    // Clear out for new message
    url_.clear();
    body_.clear();
    field_.clear();
    value_.clear();
    status_.clear();
    response_.clear();
    in_value_  = done_ = false;
  }

  // Consume HTTP data from buffer.
  // Return true if the message is complete, false if more data is needed.
  bool parse(Buffer::Instance& data, bool end_stream) {
    done_ = false;
    Buffer::RawSlice slice;
    while (data.length() && !done_) {
      data.getRawSlices(&slice, 1);
      size_t len = http_parser_execute(&parser_, &settings_,
                                       reinterpret_cast<char*>(slice.mem_), slice.len_);
      data.drain(len);
    }
    if (!done_ && data.length() && end_stream) {
      // Stream end might be end-of-content indicator, tell the parser
      http_parser_execute(&parser_, &settings_, NULL, 0);
    }
    if (parser_.http_errno) {
      const char *err = http_errno_description(static_cast<http_errno>(parser_.http_errno));
      ENVOY_CONN_LOG(error, "HTTP->AMQP parse error: {}", *conn_, err);
      throw EnvoyException("HTTP->AMQP parse error");
    }
    return done_;
  }

  // delivery for the original request and the sender link to send the response
  // @param d delivery for the request message
  // @param m the request message
  // @param s sender to send the response
  // @param c connection for logging
  void respond(proton::delivery& d, proton::message& req, proton::sender& s, Network::Connection& conn) {
    conn_ = &conn;
    response_.address(req.reply_to());
    if (!req.correlation_id().empty()) response_.correlation_id(req.correlation_id());
    int code = parser_.status_code;
    switch (code/100) {
     case 1: case 3:            // Information or Redirect
      ENVOY_CONN_LOG(debug, "HTTP '{} {}' -> AMQP ignored", conn, code, status_);
      break;

     case 2:                    // Success
      ENVOY_CONN_LOG(debug, "HTTP '{} {}' -> AMQP accepted, send response {} to request {}", conn, code, status_, response_, req);
      d.accept();
      s.send(response_);
      break;

     case 4:                    // Client error
      ENVOY_CONN_LOG(error, "HTTP '{} {}' -> AMQP rejected request {}", conn, code, status_, req);
      d.reject();               // FIXME aconway 2018-05-03: add error info
      break;

     case 5: default:           // Server or unknown error
      ENVOY_CONN_LOG(error, "HTTP '{} {}' -> AMQP undeliverable request {}", conn, code, status_, req);
      d.modify();               // FIXME aconway 2018-05-03: add error info
      break;
    }
  }

  proton::message& message() { return response_; }

 private: // http_parser callbacks

  void onUrl(const char* data, size_t length) { url_.append(data, length); } 
  void onBody(const char* data, size_t length) { body_.append(data, length); };
  void onStatus(const char* data, size_t length) { status_.append(data, length); };

  void onHeadersComplete() {
    if (in_value_) {
      // First check for headers that map to AMQP message properties
      Http::LowerCaseString lfield(field_);
      if (lfield == CONTENT_TYPE) {
        response_.content_type(value_);
      } else {
        // All other headers map to application properties.
        response_.properties().put(std::move(field_), std::move(value_));
      }
      field_.clear();
      value_.clear();
      in_value_ = false;
    }
  };

  void onHeaderField(const char* data, size_t length) {
    onHeadersComplete();      // Complete the previous header if there was one
    field_.append(data, length);
  }

  void onHeaderValue(const char* data, size_t length) {
    in_value_ = true;
    value_.append(data, length);
  }

  void onMessageComplete() {
    done_ = true;
    response_.body(std::move(body_));
    response_.address(std::move(url_));
    response_.subject(std::to_string(parser_.status_code) + " " + status_);
  }
};

// http callback wrapper templates

template <void (AmqpResponseBuilder::*f)()>
int mb_cb(http_parser *parser) {
  (static_cast<AmqpResponseBuilder*>(parser->data)->*f)();
  return 0;                     // f() will throw on error
}

template <void (AmqpResponseBuilder::*f)(const char*, size_t)>
int mb_data_cb(http_parser *parser, const char* data, size_t length) {
  (static_cast<AmqpResponseBuilder*>(parser->data)->*f)(data, length);
  return 0;                     // f() will throw on error
}

const http_parser_settings AmqpResponseBuilder::settings_ = {
  .on_message_begin = &mb_cb<&AmqpResponseBuilder::onMessageBegin>,
  .on_url = &mb_data_cb<&AmqpResponseBuilder::onUrl>,
  .on_status = &mb_data_cb<&AmqpResponseBuilder::onStatus>,
  .on_header_field = &mb_data_cb<&AmqpResponseBuilder::onHeaderField>,
  .on_header_value = &mb_data_cb<&AmqpResponseBuilder::onHeaderValue>,
  .on_headers_complete = &mb_cb<&AmqpResponseBuilder::onHeadersComplete>,
  .on_body = &mb_data_cb<&AmqpResponseBuilder::onBody>,
  .on_message_complete = &mb_cb<&AmqpResponseBuilder::onMessageComplete>,
  .on_chunk_header = nullptr,
  .on_chunk_complete = nullptr
};


/// Network::Filter that converts between AMQP messages and HTTP requests/responses.
///
/// - onData: intercepts read of AMQP data and replaces with HTTP requests
/// - onWrite: intercepts write of HTTP responses and sends AMQP messages
///
class AmqpBridge : public Network::Filter,
                 public Logger::Loggable<Logger::Id::filter>,
                 public proton::messaging_handler
{
  std::string container_id_ { "envoy." + proton::uuid::random().str() };
  std::string dynamic_prefix_ {"_#$"}; // Dynamic address prefix avoid URL/AMQP address clashes

  long dynamic_count_ {1};        // Counter to generate dynamic addresses

  Buffer::OwnedImpl amqp_in_, amqp_out_, http_in_, http_out_, empty_;
  Network::ReadFilterCallbacks* read_callbacks_;
  proton::io::connection_driver driver_;
  typedef std::pair<proton::delivery, proton::message> DeliveredMessage;
  std::deque<DeliveredMessage> requests_;
  std::unordered_map<std::string, proton::sender> senders_;
  AmqpResponseBuilder response_;

  Network::Connection& conn() { return read_callbacks_->connection(); }

 public:
  AmqpBridge(Server::Configuration::FactoryContext&) : response_() {}

  // == Envoy Network callbacks

  void initializeReadFilterCallbacks(Network::ReadFilterCallbacks& callbacks) override {
    read_callbacks_ = &callbacks;
  }

  Network::FilterStatus onNewConnection() override {
    conn().enableHalfClose(true);
    driver_.accept(proton::connection_options().handler(*this).container_id(container_id_));
    return Network::FilterStatus::Continue;
  }

  Network::FilterStatus onData(Buffer::Instance& data, bool end_stream) override {
    amqp_in_.move(data);        // Append to unprocessed AMQP data from downstream
    process(end_stream, false);
    if (http_out_.length()) {
      data.move(http_out_);     // Replace with HTTP data for upstream
    }
    if (amqp_out_.length()) {
      // FIXME aconway 2018-05-02: hack, need a filter-specific write() call that only gets
      // intercepted by downstream filters. This is a workaround for now - send an empty
      // write to wake up onWrite() so it can notice data in amqp_out_
      conn().write(empty_, false);
    }
    return Network::FilterStatus::Continue;
  }

  Network::FilterStatus onWrite(Buffer::Instance& data, bool end_stream) override {
    if (data.length()) {
      http_in_.move(data);      // Consume HTTP data
    }
    process(false, end_stream);
    if (amqp_out_.length() > 0) {
      data.move(amqp_out_);     // Replace with AMQP data
    }
    return Network::FilterStatus::Continue;
  }

  // == Proton AMQP callbacks

  void on_receiver_open(proton::receiver& r) override {
    r.open(proton::receiver_options().auto_accept(false));
  }

  void on_sender_open(proton::sender& s) override  {
    if (s.source().dynamic()) {
      // Generate a unique, dynamic, local address for responses to the connected peer.
      std::string addr = dynamic_address();
      senders_[addr] = s;
      s.open(proton::sender_options().source(proton::source_options().address(addr)));
    } else {
      // We don't allow any non-dynamic subscriptions.
      s.close(proton::error_condition(
                "amqp:not-found",
                "bad source address '" + s.source().address() + "', only dynamic sources allowed"));
    }
  }

  void on_message(proton::delivery& d, proton::message& m) override {
    try {
      HttpRequestBuilder(http_out_).message(d, m); // Forward HTTP request
      requests_.push_back(std::make_pair(d, m));
      ENVOY_CONN_LOG(debug, "AMQP request will be forwarded as HTTP: {}", conn(), m);
    } catch (const Uri::Error& e) {
      d.reject();               // FIXME aconway 2018-05-03: add condition
      ENVOY_CONN_LOG(error, "AMQP request rejected, `to` address is not a valid URI: {} {}", conn(), m);
    } catch (const std::exception& e) {
      d.reject();               // FIXME aconway 2018-05-03: add condition
      ENVOY_CONN_LOG(error, "AMQP request rejected, unknown error: {} {}", conn(), e.what(), m);
    }
  }

 private:

  void respond() {
    if (requests_.empty()) {
      ENVOY_CONN_LOG(error, "dropping unexpected HTTP->AMQP response: {}", conn(), response_.message());
    } else {
      DeliveredMessage &dm = requests_.front();
      std::string reply_to = dm.second.reply_to();
      auto i = senders_.find(reply_to);
      if (i != senders_.end()) {  // reply_to is local dynamic link
        response_.respond(dm.first, dm.second, i->second, conn());
      } else {
        // FIXME aconway 2018-05-07: route to outgoing connections... part 2
        ENVOY_CONN_LOG(error, "unknown AMQP reply-to address: {}", conn(), reply_to);
        dm.first.reject();               // FIXME aconway 2018-05-03: add condition
      }
      requests_.pop_front();
    }
  }

  void process(bool end_amqp, bool end_http) {
    while (response_.parse(http_in_, end_http)) { // HTTP parse loop
      respond();
    }
    if (end_http) {
      driver_.connection().close();
      driver_.write_close();
    }
    do {                        // AMQP event loop
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
    } while (driver_.has_events() && driver_.dispatch());
  }

  // Generate a unique, dynamic, local link source address
  std::string dynamic_address() {
    std::ostringstream addr;
    addr << dynamic_prefix_ << dynamic_count_++;
    return addr.str();
  }
};


using namespace Server::Configuration;

// Configuration and factory
class AmqpBridgeConfigFactory : public NamedNetworkFilterConfigFactory {
 public:
  Network::FilterFactoryCb createFilterFactoryFromProto(const Protobuf::Message&, FactoryContext& fc) override {
    return [this, &fc](Network::FilterManager& filter_manager) -> void {
      filter_manager.addFilter(std::make_shared<AmqpBridge>(fc));
    };
  }

  ProtobufTypes::MessagePtr createEmptyConfigProto() override {
    return ProtobufTypes::MessagePtr{new Envoy::ProtobufWkt::Empty()};
  }

  std::string name() override { return NAME; }

  Network::FilterFactoryCb createFilterFactory(const Json::Object&, FactoryContext&) override {
    NOT_IMPLEMENTED;
  }
};

// Static registration for the filter. @see RegisterFactory.
static Registry::RegisterFactory<AmqpBridgeConfigFactory, NamedNetworkFilterConfigFactory> registered_;

}}}}
