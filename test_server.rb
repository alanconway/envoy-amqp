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
class TestHTTPServer < WEBrick::HTTPServer
  def initialize
    @requests = Queue.new
    #out = WEBrick::Log::new("/dev/null", 7)
    out = WEBrick::Log.new("stderr")
    # TODO aconway 2018-05-04: port is hard coded in amqp_bridge.yaml
    super(:Port => 8000, :Logger => out, :AccessLog => out)
    mount_proc "/" do |req, res|
      @req = req.dup
      res.status = req["test-status"] || 200
      req.each { |k,v| res[$1] = v if /test-h-(.*)/.match(k) }
      res.body = "response=#{req.body}"
    end
    @thread = Thread.new { start }
  end

  def join() @thread.join; end

  attr_reader :req

  @@instance = TestHTTPServer.new
  def self.get() @@instance; end
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

class TestAMQPClient

  def initialize(requests, opts={})
    @netaddr = opts[:netaddr] || ":18000"
    @linkaddr = opts[:linkaddr]
    @vhost = opts[:vhost]
    @reply_to = opts[:reply_to]
    @requests = requests
    @responses = []
    raise "No requests" if requests.empty?
    Container.new(self, "#{__FILE__}-#{Process::pid}").run()
  end

  attr_reader :responses, :reply_to

  def on_error(e) raise e; end
 
  def on_container_start(cont)
    c = cont.connect(@netaddr, { :virtual_host => @vhost })
    @sender = c.open_sender(@linkaddr)
    @receiver = c.open_receiver({:dynamic => true})
  end

  def send_request
    req = @requests.shift
    req.reply_to = @reply_to || @receiver.remote_source.address
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
    # TODO aconway 2018-05-07: need full disposition
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

class EnvoyAmqpServerTest < MiniTest::Test

  def request(*args) TestAMQPClient.request(*args); end
  def requests(*args) TestAMQPClient.requests(*args);  end
  def server_req() TestHTTPServer.get.req; end

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
    s = server_req
    assert_equal "POST", s.request_method, s
    assert_equal a, s.unparsed_uri, s
    assert_equal "", s["host"], s
    assert_equal "5", s["content-length"], s
    assert_equal "thing", s["content-type"], s
    assert_equal "blang", s["bling"], s
    assert_equal "polo", s["marco"], s


    # Check the AMQP response
    assert_equal req.reply_to, res.address
    assert_equal "200 OK", res.subject
    assert_equal "response=hello", res.body
    assert_equal "stuff", res.content_type
    assert_equal "envoy", res["server"], res
    assert_equal "pong", res["ping"], res
  end

  def test_methods
    addr = "/#{__method__}"
    request(Message.new("blubber", {:subject=>"POST", :address=>addr+"/x"}))
    assert_equal "POST", server_req.request_method
    request(Message.new(nil, {:subject=>"GET", :address=>addr+"/y"}))
    assert_equal "GET", server_req.request_method
  end

  def test_address_uri
    a = "/#{__method__}"
    request(Message.new(nil, {:subject=>"GET", :address=>a+"/x"}))
    assert_equal "#{a}/x", server_req.unparsed_uri
    # Fall back to link address if there is no to address
    request(Message.new(nil, {:subject=>"GET"}), {:linkaddr => a+"/l"})
    assert_equal "#{a}/l", server_req.unparsed_uri
  end

  def test_http_errors
    # TODO aconway 2018-06-06: review error message contents
    a = "/#{__method__}"
    m = Message.new("", { :address => a, :correlation_id => 42 })

    # Errors detected at the bridge
    m.subject = "BAD_METHOD"
    res = request(m)
    assert_equal ["400 Bad Request", 42], [res.subject, res.correlation_id], res

    m.subject = "GET"
    m.address = "!@#!@!bad_uri"
    res = request(m)
    assert_equal ["400 Bad Request", 42], [res.subject, res.correlation_id], res

    m.address = ""
    res = request(m)
    assert_equal ["400 Bad Request", 42], [res.subject, res.correlation_id], res

    # Errors returned by the HTTP server
    m.address = a
    m.properties = { "test-status"=>"404"}
    res = request(m)
    assert_equal ["404 Not Found", 42], [res.subject, res.correlation_id], res

    m.properties = { "test-status"=>"505"}
    res = request(m)
    assert_equal ["505 HTTP Version Not Supported", 42], [res.subject, res.correlation_id], res
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

  def test_body_types
    a = "/#{__method__}/"
    assert_equal "400 Bad Request", request(Message.new([:not_a_legal_body], { :address=>a, :subject=>"POST" })).subject
  end

  # Version of the test client that doesn't open a sender link, but allows it to open from remote.
  class TestRequestLinkAMQPClient < TestAMQPClient
    def initialize(requests, opts={})
      opts[:netaddr] ||= ":18001"
      super(requests, opts)
    end

    def on_container_start(cont)
      c = cont.connect(@netaddr, { :virtual_host => @vhost })
      # Don't open sender, it will be opened by the other end.
      @receiver = c.open_receiver({:dynamic => true})
    end

    def on_sender_open(s)
      @sender = s
      send_request
    end

    def send_request
      return unless @sender && @sender.open? && @receiver && @receiver.open?
      req = @requests.shift
      req.reply_to = @receiver.remote_source.address
      @sender.send(req)
    end

    attr_reader :sender
  end

  def test_request_relay
    a = "/#{__method__}/"
    client = TestRequestLinkAMQPClient.new([Message.new("body", {:address=>a, :subject=>"POST"})])
    res = client.responses[0]
    assert_equal "200 OK", res.subject
    assert_equal "response=body", res.body

    l = client.sender
    assert_equal "amqp_in2", l.connection.container_id
    assert_equal "amqp_in2-link", l.remote_source.address
  end

  def test_bad_reply_to
    a = "/#{__method__}/"
    client = TestAMQPClient.new([Message.new("", {:address=>a, :subject=>"GET"})], {:reply_to=>"BAD"})
    assert_equal Tracker::REJECTED, client.responses[0]
  end
end

MiniTest.after_run do
  TestHTTPServer.get.shutdown
end

