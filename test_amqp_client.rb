#!/usr/bin/env ruby
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
require 'webrick'

include Qpid::Proton

# Start a global web server in a thread, used by all tests
class TestServer < WEBrick::HTTPServer
  def initialize
    #out = WEBrick::Log::new("/dev/null", 7)
    out = WEBrick::Log.new("stderr")
    # FIXME aconway 2018-05-04: port is hard coded in amqp_bridge.yaml
    super(:Port => 8000, :Logger => out, :AccessLog => out)
    mount_proc "/" do |req, res|
      @req = req.dup
      res.status = req["test-status"] || 200
      req.each { |k,v| res[$1] = v if /test-h-(.*)/.match(k) }
      res.body = "response=#{req.body}"
    end
    @thread = Thread.new { start }
    @lock = Mutex.new
  end

  attr_reader :req

  def stop
    super
    @thread.join
  end

  @@instance = TestServer.new
  def self.get() @@instance; end
  def self.stop() @@instance.stop if @@instance; end
end

# MessagingHandler that raises in on_error to catch unexpected errors
class ExceptionMessagingHandler
  def on_error(e) raise e; end
end

class Rejected < RuntimeError; end
class Modified < RuntimeError; end
class Released < RuntimeError; end

def raise_status(status)
  case status
  when Tracker::REJECTED then raise Rejected
  when Tracker::MODIFIED then raise Modified
  when Tracker::RELEASED then raise Released
  when Tracker::ACCEPTED then ;
  else raise "unknown status #{status}"
  end
end

class TestClient < ExceptionMessagingHandler
  include Qpid::Proton

  def initialize(requests, opts={})
    @netaddr = opts[:netaddr] || ":10000"
    @linkaddr = opts[:linkaddr]
    @vhost = opts[:vhost]
    @id = opts[:id] || "amqp_client_test"
    @requests = requests
    @responses = []
    Container.new(self, @id).run()
  end

  attr_reader :responses, :reply_to

  def on_container_start(cont)
    c = cont.connect(@netaddr, { :virtual_host => @vhost })
    @sender = c.open_sender(@linkaddr)
    @receiver = c.open_receiver({:dynamic => true})
  end

  def send_request
    req = @requests.shift
    req.reply_to = @receiver.remote_source.address
    @sender.send(req)
  end

  def on_receiver_open(receiver)
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
    response message
  end

  def on_tracker_settle(tracker)
    # FIXME aconway 2018-05-07: need full disposition
    response tracker.state unless tracker.state == Tracker::ACCEPTED
  end

  # Send one request return the response (message or disposition)
  def self.request(m, opts={})
    res = self.new([m], opts).responses[0];
    raise_status res unless res.is_a?(Message)
    res
  end

  # Send multiple requests return all the  responses (message or disposition)
  def self.requests(ms, opts={})
    self.new(ms, opts).responses
  end

end

class AmqpBridgeTest < MiniTest::Test
  include Qpid::Proton


  def request(*args) TestClient.request(*args); end
  def requests(*args) TestClient.requests(*args);  end
  def server_req() TestServer.get.req; end

  def test_mapping
    a = "/#{__method__}"
    req = Message.new("hello",
                      {:subject=>"POST", :address=>a, :content_type=>"thing",
                       :properties=>{"bling"=>"blang", "marco"=>"polo",
                                     "test-h-content-type"=>"stuff",
                                     "test-h-ping"=>"pong",
                                    }})
    res = request(req)

    # Check the HTTP request
    assert_equal "POST", server_req.request_method
    assert_equal a, server_req.unparsed_uri
    headers = server_req.to_enum(:each).to_a
    assert_includes headers, ["host", ""]
    assert_includes headers, ["content-length", "5"]
    assert_includes headers, ["content-type", "thing"]
    assert_includes headers, ["bling", "blang"]
    assert_includes headers, ["marco", "polo"]


    # Check the AMQP response
    assert_equal req.reply_to, res.address
    assert_equal "200 OK", res.subject
    assert_equal "response=hello", res.body
    assert_equal "stuff", res.content_type
    props = res.properties.to_a
    assert_includes props, ["server", "envoy"]
    assert_includes props, ["content-length", res.body.size.to_s]
    assert_includes props, ["ping", "pong"]
  end

  def test_methods
    addr = "/#{__method__}"
    res = request(Message.new("blubber", {:subject=>"POST", :address=>addr+"/x"}))
    assert_equal "POST", server_req.request_method
    res = request(Message.new(nil, {:subject=>"GET", :address=>addr+"/y"}))
    assert_equal "GET", server_req.request_method
  end

  def test_address_uri
    a = "/#{__method__}"
    res = request(Message.new(nil, {:subject=>"GET", :address=>a+"/x"}))
    assert_equal "#{a}/x", server_req.unparsed_uri
    # Fall back to link address if there is no to address
    res = request(Message.new(nil, {:subject=>"GET"}), {:linkaddr => a+"/l"})
    assert_equal "#{a}/l", server_req.unparsed_uri
  end

  def test_http_errors
    a = "/#{__method__}"
    m = Message.new("", { :address => a })
    # Errors detected at the bridge
    m.subject = "BAD_METHOD"
    assert_raises(Rejected) { request(m) }
    m.subject = "GET"
    m.address = "bad_uri"
    assert_raises(Rejected) { request(m) }
    m.address = ""
    assert_raises(Rejected) { request(m) }
    # Errors returned by the HTTP server
    m.address = a
    m.properties = { "test-status"=>"404"}
    assert_raises(Rejected) { request(m) }
    m.properties = { "test-status"=>"505"}
    assert_raises(Modified) { request(m) }
    # FIXME aconway 2018-05-14: no reply_to is an error? Maybe not for POST.
  end

  # "persistent" in HTTP-speak means sending more than one request per connection.
  def test_persistent
    a = "/#{__method__}/"
    requests = ["a","b","c","d"].collect { |x| Message.new(x, {:subject=>"POST", :address=>a+x}) }
    responses = requests.collect { |req| request(req) }
    assert_equal requests.collect { |m| "response=#{m.body}" },
                 responses.collect { |m| m.body }
  end

  # "pipeline" in HTTP-speak means sending multiple requests before receiving a response.
  def test_pipeline
    a = "/#{__method__}/"
    data = ["a","b","c", "d"]
    responses = requests(data.map { |x| Message.new(x, {:subject=>"POST", :address=>a+x})})
    assert_equal(data.map { |x| "response=#{x}" }, responses.map { |m| m.body })
  end

end

MiniTest.after_run do
  TestServer.stop
end

