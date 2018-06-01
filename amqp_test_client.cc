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
 *
 */

// FIXME aconway 2018-05-07: BROKEN! Complete and automate a C++ test.

// Simple AMQP client to exercise AMQP request/response -> HTTP server

#include "proton/connection.hpp"
#include "proton/container.hpp"
#include "proton/delivery.hpp"
#include "proton/message.hpp"
#include "proton/messaging_handler.hpp"
#include "proton/receiver.hpp"
#include "proton/receiver_options.hpp"
#include "proton/sender.hpp"
#include "proton/source_options.hpp"
#include "proton/source_options.hpp"

#include <deque>
#include <iostream>
#include <sstream>

// FIXME aconway 2018-05-04: needed in C++ API
const char *pn_disposition_type_name(uint64_t d) {
  switch(d) {
   case PN_RECEIVED: return "received";
   case PN_ACCEPTED: return "accepted";
   case PN_REJECTED: return "rejected";
   case PN_RELEASED: return "released";
   case PN_MODIFIED: return "modified";
   default: return "unknown";
  }
}

class AmqpClient : public proton::messaging_handler {
 private:
  std::string netaddr_, subject_, url_;
  int count_;
  proton::sender sender_;
  proton::receiver receiver_;
  std::deque<proton::message> responses_;

 public:
  AmqpClient(const std::string &netaddr, const std::string& subject, const std::string& url, int count) :
    netaddr_(netaddr), subject_(subject), url_(url), count_(count)  {}

  void on_container_start(proton::container &c) override {
    proton::connection conn = c.connect(netaddr);
    sender = conn.open_sender(url);
    auto opts = proton::receiver_options().source(proton::source_options().dynamic(true));
    receiver = conn.open_receiver("", opts);
  }

  void on_receiver_open(proton::receiver &) override {
    std::cout << "receiver_open: " << receiver.source().address() << std::endl;
    send_request();
  }

  void on_message(proton::delivery &d, proton::message &m) override {
    responses_.push_back(m);
    std::cout << m << std::endl;
    if (--count_ > 0) {
      send_request();
    }
  }

  void on_tracker_settle(proton::tracker &t) {
    if (t.state() != proton::tracker::ACCEPTED) {
      // We will never get a response, put a fake message on responses_ for the test
      // FIXME aconway 2018-05-04: need disposition condition from C++ API.
      responses_.push_back(proton::message(std::string("FAILED") + pn_disposition_type_name(t.state())));
    }
  }

  void send_request() {
    auto& req = message(requests.front();
    req.reply_to(receiver.source().address());
    sender.send(req);
    // FIXME aconway 2018-05-03: verify correlation id transfer
    // test with/without correlation id
  }
};

// Usage [HOST:PORT [SUBJECT [URL [BODY [COUNT]]]]]
int main (int argc, char** argv) {
  int arg = 1;
  std::string netaddr = (argc > arg) ? argv[arg++] : "";
  std::string subject = (argc > arg) ? argv[arg++] : "POST";
  std::string url = (argc > arg) ? argv[arg++] : "/";
  std::string body = (argc > arg) ? argv[arg++] : NULL;
  int count  (argc > arg) ? argv[arg++] : 1;

  AmqpClient client(netaddr, subject);
  // Bare message will be POST to the link address
  client.requests.push_back(proton::message("hello world"));
  proton::message m("hello other world");
  m.address("/message-url");
  client.requests.push_back(m);
  m = proton::message();
  m.subject("GET");
  m.address("/get-url");
  client.requests.push_back(m);
  proton::container(client).run();
}
