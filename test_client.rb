#
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
require 'net/http'
require 'zlib'

include Qpid::Proton
Thread::abort_on_exception = true

# Back-end AMQP request/response server in a thread.
class TestAMQPServer
  def initialize
    @container = Container.new(self, "#{__FILE__}-#{Process.pid}")
    @requests = Queue.new       # Let tests examine the AMQP request converted from HTTP
    @senders = {}
    @listener = @container.listen ":5672" # TODO aconway 2018-05-31: hard-coded in .yaml
    @thread = Thread.new { @container.run }
  end

  attr_reader :requests

  def on_error(e) raise e; end

  def on_sender_open(sender)
    src = sender.remote_source
    raise "expected dynamic source: #{sender.name} #{src.inspect}" unless src.dynamic?
    addr = "#{sender.connection.container_id}/#{sender.name}"
    sender.open({:source => addr})
    @senders[addr] = sender
  end

  def on_message(delivery, request)
    @requests << request
    case request['outcome']
    when 'reject' then delivery.reject
    when 'release' then delivery.release({ :failed => false })
    when 'modify' then delivery.release({ :failed => true }) # Add annotations
    else
      delivery.accept
      sender = @senders[request.reply_to] or raise "no sender #{request.inspect}"
      if request.content_type == "amqp" # body is an encoded AMQP response
        response = tee(Message.new) { |m| m.decode(request.body) }
      else
        response = Message.new(request.address)
      end
      response.address = request.reply_to
      response.correlation_id = request.correlation_id
      sender.send(response)
    end
  end

  def stop
    @container.stop
    @thread.join
  end

  @@instance = self.new
  def self.get() @@instance; end
  def self.stop() @@instance.stop if @@instance; end
end

class EnvoyAmqpClientTest < MiniTest::Test

  def assert_hash_contains(want, got, msg=nil)
    assert(got.merge(want) == got, msg || " #{want} not found in #{got}")
  end

  def setup()
    @http = Net::HTTP.new("", 15672);    # TODO aconway 2018-05-31: hard-coded in .yaml
    @http.start
    @server = TestAMQPServer.get
    @a = "/#{name}/"            # Use test name as address for traceability
  end

  def teardown()
    @http.finish
    TestAMQPServer.get.requests.clear # Clear left-over requests
  end

  def server_pop() @server.requests.pop; end

  def test_get
    res = @http.get(@a)
    assert_equal ["200", @a], [res.code, res.body]
    s = server_pop
    assert_equal [@a, "GET"], [s.address, s.subject]
    refute_nil s.reply_to
    refute_nil s.correlation_id
    assert_nil s.content_type
  end

  def test_post
    r = @http.post(@a, "foo", { "x" => "y", "content-type" => "footype"})
    assert_equal [@a], [r.body]

    s = server_pop
    assert_equal [@a, "POST", "footype", "foo"], [s.address, s.subject, s.content_type, s.body]
    assert_hash_contains({"x"=>"y"}, s.properties)
  end

  def test_response
    # send a request with an encoded response, verify it is properly mapped to HTTP
  end

  def test_errors
    # Errors from the AMQP server
    r = @http.post(@a, "foo", { 'outcome'=>'reject' })
    assert_equal ["502", "Bad Gateway", "rejected"], [r.code, r.message, r.body]
    r = @http.post(@a, "foo", { 'outcome'=>'release' })
    assert_equal ["502", "Bad Gateway", "released"], [r.code, r.message, r.body]
    r = @http.post(@a, "foo", { 'outcome'=>'modify' })
    assert_equal ["502", "Bad Gateway", "modified"], [r.code, r.message, r.body]
  end
end

MiniTest.after_run do
  TestAMQPServer.stop
end
