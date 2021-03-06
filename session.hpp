#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/strand.hpp>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

namespace beast = boost::beast;         // from <boost/beast.hpp>
namespace http = beast::http;           // from <boost/beast/http.hpp>
namespace net = boost::asio;            // from <boost/asio.hpp>
using tcp = boost::asio::ip::tcp;       // from <boost/asio/ip/tcp.hpp>

//------------------------------------------------------------------------------

// Report a failure
void
fail(beast::error_code ec, char const* what)
{
  std::cerr << what << ": " << ec.message() << "\n";
}

// Performs an HTTP GET and prints the response
class session : public std::enable_shared_from_this<session>
{
  tcp::resolver resolver_;
  beast::tcp_stream stream_;
  beast::flat_buffer buffer_; // (Must persist between reads)
  http::request<http::empty_body> req_;
  //http::response<http::string_body> res_;
  http::response_parser<http::buffer_body> parser_;
  char body_buffer[131072];
  
  std::function<void(http::header<false,http::fields>&)> header_cb;
  std::function<void(std::string_view&& data)> body_cb;
  
public:
  // Objects are constructed with a strand to
  // ensure that handlers do not execute concurrently.
  explicit
  session(net::io_context& ioc)
    : resolver_(net::make_strand(ioc))
    , stream_(net::make_strand(ioc))
  {
    parser_.body_limit(std::numeric_limits<uint64_t>::max());
    header_cb = [](http::header<false,http::fields>& hdr) {
		  std::cerr << hdr;
		};
    body_cb = [](std::string_view&& data) {
		std::cerr << data;
	      };
    
  }
  
  // Start the asynchronous operation
  void
  run(
      char const* host,
      char const* port,
      char const* target,
      int version)
  {
    // Set up an HTTP GET request message
    req_.version(version);
    req_.method(http::verb::get);
    req_.target(target);
    req_.set(http::field::host, host);
    req_.set(http::field::user_agent, BOOST_BEAST_VERSION_STRING);
    
    // Look up the domain name
    resolver_.async_resolve(
			    host,
			    port,
			    beast::bind_front_handler(
						      &session::on_resolve,
						      shared_from_this()));
  }
  
  void
  on_resolve(
	     beast::error_code ec,
	     tcp::resolver::results_type results)
  {
    if(ec)
      return fail(ec, "resolve");
    
    // Set a timeout on the operation
    stream_.expires_after(std::chrono::seconds(10));
    
    // Make the connection on the IP address we get from a lookup
    stream_.async_connect(
			  results,
			  beast::bind_front_handler(
						    &session::on_connect,
						    shared_from_this()));
  }
  
  void
  on_connect(beast::error_code ec, tcp::resolver::results_type::endpoint_type)
  {
    if(ec)
      return fail(ec, "connect");
    
    // Set a timeout on the operation
    // Suspect of terminating the stream with "partial message" error
    stream_.expires_after(std::chrono::seconds(10));
    
    // Send the HTTP request to the remote host
    http::async_write(stream_, req_,
		      beast::bind_front_handler(
						&session::on_write,
						shared_from_this()));
  }
  
  void
  on_write(
	   beast::error_code ec,
	   std::size_t bytes_transferred)
  {
    boost::ignore_unused(bytes_transferred);
    
    if(ec)
      return fail(ec, "write");

    // Remove timeout; may terminate long download
    stream_.expires_never();
    
    // Receive the HTTP response
    http::async_read_header(stream_, buffer_, parser_,
			    beast::bind_front_handler(
						      &session::on_read_header,
						      shared_from_this()));
    
    //http::async_read(stream_, buffer_, res_,
    //    beast::bind_front_handler(
    //        &session::on_read,
    //        shared_from_this()));
  }
  
  /*
    void
    on_read(
    beast::error_code ec,
    std::size_t bytes_transferred)
    {
    boost::ignore_unused(bytes_transferred);
    
    if(ec)
    return fail(ec, "read");
    
    // Write the message to standard out
    std::cout << res_ << std::endl;
    
    // Gracefully close the socket
    stream_.socket().shutdown(tcp::socket::shutdown_both, ec);
    
    // not_connected happens sometimes so don't bother reporting it.
    if(ec && ec != beast::errc::not_connected)
    return fail(ec, "shutdown");
    
    // If we get here then the connection is closed gracefully
    }*/

 
  void
  on_read_header(
		 beast::error_code ec,
		 std::size_t bytes_transferred)
  {
    if(ec)
      return fail(ec, "read header");
    
    if(header_cb)
      header_cb(parser_.get().base());
    
    read_body();
  }
  
  void
  read_body()
  {
    parser_.get().body().data = body_buffer;
    parser_.get().body().size = sizeof body_buffer;
    http::async_read(stream_, buffer_, parser_,
		     beast::bind_front_handler(
					       &session::on_read_body,
					       shared_from_this()));
  }
  
  void
  on_read_body(
	       beast::error_code ec,
	       std::size_t bytes_transferred)
  {
    boost::ignore_unused(bytes_transferred);
    
    if(ec == http::error::need_buffer)
      ec = {};
    
    if(ec)
      return fail(ec, "read body");
    
    if(body_cb)
      body_cb(std::string_view(body_buffer, sizeof body_buffer - parser_.get().body().size));
    
    if(parser_.is_done())
      {
	// Write the message to standard out
	//std::cout << res_ << std::endl;
	
	// Gracefully close the socket
	stream_.socket().shutdown(tcp::socket::shutdown_both, ec);
	
	// not_connected happens sometimes so don't bother reporting it.
	if(ec && ec != beast::errc::not_connected)
	  {
	    return fail(ec, "shutdown");
	  }
	// If we get here then the connection is closed gracefully
	
      }
    else
      {
        read_body();
      }
  }
  
};
