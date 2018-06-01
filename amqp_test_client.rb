# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.


require 'minitest/autorun'
require 'minitest/unit'
require 'qpid_proton'

# MessagingHandler that raises in on_error to catch unexpected errors
class ExceptionMessagingHandler
  def on_error(e) raise e; end
end

class TestClient < ExceptionMessagingHandler
  include Qpid::Proton

  def initialize(requests, opts={})
    @netaddr = opts[:netaddr] || ":10000"
    @linkaddr = opts[:linkaddr] || "/amqp_bridge"
    @id = opts[:id] || "amqp_client_test"
    @requests = requests
    @responses = []
    Container.new(self, @id).run()
  end

  attr_reader :responses

  def on_container_start(cont)
    c = cont.connect(@netaddr)
    @sender = c.open_sender(@linkaddr)
    @receiver = c.open_receiver({:dynamic => true})
  end

  def send_request
    req = @requests.shift
    req.reply_to = @receiver.remote_source.address
    @sender.send(req)
    STDERR.puts "FIXME request: #{req}"
  end

  def on_receiver_open(receiver)
    STDERR.puts "FIXME receiver open: #{receiver.remote_source.address}"
    send_request
  end

  def response(m)
    @responses << m             # Message or delivery state
    if @requests.empty?
      @sender.connection.close
    else
      send_request
    end
  end

  def on_message(delivery, message)
    STDERR.puts "FIXME on_message: #{message}"
    response message
  end

  def on_tracker_settle(tracker)
    response Tracker::State.name(tracker.state)
  end
end

class ContainerTest < MiniTest::Test
  include Qpid::Proton

  def test_single_get
    m = Message.new
    m.subject = "GET"           #FIXME aconway 2018-05-04: message constructor?
    m.address =  "/"
    STDERR.puts "FIXME #{m}"
    assert_equal [], TestClient.new([m]).responses
  end
end
