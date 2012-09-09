/*

Copyright (c) 2012, Arvid Norberg, Magnus Jonsson
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#include <deque>
#include <getopt.h> // for getopt_long
#include <stdlib.h> // for daemon()
#include <syslog.h>
#include <boost/bind.hpp>

#include "libtorrent/session.hpp"
#include "libtorrent/thread.hpp"
#include "libtorrent/socket.hpp"
#include "libtorrent/io.hpp"
#include "libtorrent/puff.hpp"
#include "deluge.hpp"
#include "rencode.hpp"
#include <zlib.h>

using namespace libtorrent;

namespace io = libtorrent::detail;

enum rpc_type_t
{
	RPC_RESPONSE = 1,
	RPC_ERROR = 2,
	RPC_EVENT = 3
};

deluge::deluge(session& s, std::string pem_path)
	: m_ses(s)
	, m_listen_socket(NULL)
	, m_accept_thread(NULL)
	, m_context(m_ios, boost::asio::ssl::context::sslv23)
	, m_shutdown(false)
{
	m_context.set_options(
		boost::asio::ssl::context::default_workarounds
		| boost::asio::ssl::context::no_sslv2
		| boost::asio::ssl::context::single_dh_use);
	error_code ec;
//	m_context.set_password_callback(boost::bind(&server::get_password, this));
	m_context.use_certificate_chain_file(pem_path.c_str(), ec);
	if (ec)
	{
		fprintf(stderr, "use cert: %s\n", ec.message().c_str());
		return;
	}
	m_context.use_private_key_file(pem_path.c_str(), boost::asio::ssl::context::pem, ec);
	if (ec)
	{
		fprintf(stderr, "use key: %s\n", ec.message().c_str());
		return;
	}
//	m_context.use_tmp_dh_file("dh512.pem");
}

deluge::~deluge()
{
}

void deluge::accept_thread(int port)
{
	socket_acceptor socket(m_ios);
	m_listen_socket = &socket;

	error_code ec;
	socket.open(tcp::v4(), ec);
	if (ec)
	{
		fprintf(stderr, "open: %s\n", ec.message().c_str());
		return;
	}
	socket.set_option(socket_acceptor::reuse_address(true), ec);
	if (ec)
	{
		fprintf(stderr, "reuse address: %s\n", ec.message().c_str());
		return;
	}
	socket.bind(tcp::endpoint(address_v4::any(), port), ec);
	if (ec)
	{
		fprintf(stderr, "bind: %s\n", ec.message().c_str());
		return;
	}
	socket.listen(5, ec);
	if (ec)
	{
		fprintf(stderr, "listen: %s\n", ec.message().c_str());
		return;
	}

	TORRENT_ASSERT(m_threads.empty());
	for (int i = 0; i < 5; ++i)
		m_threads.push_back(new thread(boost::bind(&deluge::connection_thread, this)));

	do_accept();
	m_ios.run();

	for (std::vector<thread*>::iterator i = m_threads.begin()
		, end(m_threads.end()); i != end; ++i)
	{
		(*i)->join();
		delete *i;
	}

	mutex::scoped_lock l(m_mutex);
	m_threads.clear();

	for (std::vector<ssl_socket*>::iterator i = m_jobs.begin()
		, end(m_jobs.end()); i != end; ++i)
	{
		delete *i;
	}
	m_jobs.clear();
}

void deluge::do_accept()
{
	TORRENT_ASSERT(!m_shutdown);
	ssl_socket* sock = new ssl_socket(m_ios, m_context);
	m_listen_socket->async_accept(sock->lowest_layer(), boost::bind(&deluge::on_accept, this, _1, sock));
}

void deluge::on_accept(error_code const& ec, ssl_socket* sock)
{
	if (ec)
	{
		delete sock;
		do_stop();
		return;
	}

	fprintf(stderr, "accepted connection\n");
	mutex::scoped_lock l(m_mutex);
	m_jobs.push_back(sock);
	m_cond.signal(l);
	l.unlock();

	do_accept();
}

struct handler_map_t
{
	char const* method;
	char const* args;
	void (deluge::*fun)(rtok_t const*, char const* buf, rencoder&);
};

handler_map_t handlers[] =
{
	{"daemon.login", "[ss]{}", &deluge::handle_login},
	{"daemon.set_event_interest", "[[s]]{}", &deluge::handle_set_event_interest},
	{"daemon.info", "[]{}", &deluge::handle_info},
	{"core.get_config_value", "[s]{}", &deluge::handle_get_config_value},
};

void deluge::incoming_rpc(rtok_t const* tokens, char const* buf, rencoder& output)
{
	printf("<== ");
	print_rtok(tokens, buf);
	printf("\n");

	// RPCs are always 4-tuples, anything else is malformed
	// first the request-ID
	// method name
	// arguments
	// keyword (named) arguments
	if (validate_structure(tokens, "[is[]{}]") == false)
	{
		int id = -1;
		if (tokens[1].type() == type_integer)
			id = tokens[1].integer(buf);

		output_error(id, "invalid RPC format", output);
		return;
	}

	std::string method = tokens[2].string(buf);

	for (int i = 0; i < sizeof(handlers)/sizeof(handlers[0]); ++i)
	{
		if (handlers[i].method != method) continue;

		if (!validate_structure(tokens+3, handlers[i].args))
		{
			output_error(tokens[1].integer(buf), "invalid arguments", output);
			return;
		}

		(this->*handlers[i].fun)(tokens, buf, output);
		return;
	}

	output_error(tokens[1].integer(buf), "unknown method", output);
}

void deluge::handle_login(rtok_t const* tokens, char const* buf, rencoder& output)
{
	int id = tokens[1].integer(buf);

	// [ RPC_RESPONSE, req-id, [5] ]

	output.append_list(3);
	output.append_int(RPC_RESPONSE);
	output.append_int(id);
	output.append_list(1);
	output.append_int(5); // auth-level
}

void deluge::handle_set_event_interest(rtok_t const* tokens, char const* buf, rencoder& output)
{
	int id = tokens[1].integer(buf);

	// [ RPC_RESPONSE, req-id, [True] ]

	output.append_list(3);
	output.append_int(RPC_RESPONSE);
	output.append_int(id);
	output.append_list(1);
	output.append_bool(true); // success
}

void deluge::handle_info(rtok_t const* tokens, char const* buf, rencoder& output)
{
	int id = tokens[1].integer(buf);

	// [ RPC_RESPONSE, req-id, ["1.0"] ]

	output.append_list(3);
	output.append_int(RPC_RESPONSE);
	output.append_int(id);
	output.append_list(1);
	output.append_string(m_ses.get_settings().get_str(settings_pack::user_agent)); // version
}

void deluge::handle_get_config_value(rtok_t const* tokens, char const* buf, rencoder& out)
{
	int id = tokens[1].integer(buf);
	if (tokens[3].type() != type_list
		|| tokens[3].num_items() < 1
		|| tokens[4].type() != type_string)
	{
		output_error(id, "invalid argument", out);
		return;
	}
	std::string config_name = tokens[4].string(buf);

	// map deluge-names to libtorrent-names
	if (config_name == "max_download_speed")
		config_name = "download_rate_limit";
	else if (config_name == "max_upload_speed")
		config_name = "upload_rate_limit";

	int name = setting_by_name(config_name);
	if (name == -1)
	{
		output_error(id, "unknown configuration", out);
		return;
	}

	aux::session_settings set = m_ses.get_settings();
	
	// [ RPC_RESPONSE, req-id, [<config value>] ]

	out.append_list(3);
	out.append_int(RPC_RESPONSE);
	out.append_int(id);
	out.append_list(1);
	switch (name & settings_pack::index_mask)
	{
		case settings_pack::string_type_base:
			out.append_string(set.get_str(name));
			break;
		case settings_pack::int_type_base:
			out.append_int(set.get_int(name));
			break;
		case settings_pack::bool_type_base:
			out.append_bool(set.get_bool(name));
			break;
	};
}

void deluge::output_error(int id, char const* msg, rencoder& out)
{
	// [ RPC_ERROR, req-id, [msg, args, trace] ]

	out.append_list(3);
	out.append_int(RPC_ERROR);
	out.append_int(id);
	out.append_list(3);
	out.append_string(msg); // exception name
	out.append_string(""); // args
	out.append_string(""); // stack-trace
}

void deluge::connection_thread()
{
	mutex::scoped_lock l(m_mutex);
	while (!m_shutdown)
	{
		l.lock();
		while (!m_shutdown && m_jobs.empty())
			m_cond.wait(l);

		if (m_shutdown) return;

		fprintf(stderr, "connection thread woke up: %d\n", int(m_jobs.size()));
		
		ssl_socket* sock = m_jobs.front();
		m_jobs.erase(m_jobs.begin());
		l.unlock();

		error_code ec;
		sock->handshake(boost::asio::ssl::stream_base::server, ec);
		if (ec)
		{
			fprintf(stderr, "ssl handshake: %s\n", ec.message().c_str());
			sock->lowest_layer().close(ec);
			delete sock;
			continue;
		}
		fprintf(stderr, "SSL handshake done\n");

		std::vector<char> buffer;
		std::vector<char> inflated;
		buffer.resize(2048);
		do
		{
			int buffer_use = 0;
			error_code ec;

			int ret;
			z_stream strm;

read_some_more:
			TORRENT_ASSERT(buffer.size() > 0);
			TORRENT_ASSERT(buffer.size() - buffer_use > 0);
			ret = sock->read_some(asio::buffer(&buffer[buffer_use]
				, buffer.size() - buffer_use), ec);
			if (ec)
			{
				fprintf(stderr, "read: %s\n", ec.message().c_str());
				break;
			}
			TORRENT_ASSERT(ret > 0);
			fprintf(stderr, "read %d bytes (%d/%d)\n", int(ret), buffer_use, int(buffer.size()));

			buffer_use += ret;
	
parse_message:
			// assume no more than a 1:10 compression ratio
			inflated.resize(buffer_use * 10);

			memset(&strm, 0, sizeof(strm));
			ret = inflateInit(&strm);
			if (ret != Z_OK)
			{
				fprintf(stderr, "inflateInit failed: %d\n", ret);
				break;
			}
			strm.next_in = (Bytef*)&buffer[0];
			strm.avail_in = buffer_use;

			strm.next_out = (Bytef*)&inflated[0];
			strm.avail_out = inflated.size();

			ret = inflate(&strm, Z_NO_FLUSH);

			// TODO: in some cases we should just abort as well
			if (ret != Z_STREAM_END)
			{
				if (buffer_use + 512 > buffer.size())
				{
					// don't let the client send infinitely
					// big messages
					if (buffer_use > 1024 * 1024)
					{
						fprintf(stderr, "compressed message size exceeds 1 MB\n");
						break;
					}
					// make sure we have enough space in the
					// incoming buffer.
					buffer.resize(buffer_use + buffer_use / 2 + 512);
				}
				inflateEnd(&strm);
				fprintf(stderr, "inflate: %d\n", ret);
				goto read_some_more;
			}

			// truncate the out buffer to only contain the message
			inflated.resize(inflated.size() - strm.avail_out);

			int consumed_bytes = (char*)strm.next_in - &buffer[0];
			TORRENT_ASSERT(consumed_bytes > 0);

			inflateEnd(&strm);

			rtok_t tokens[200];
			ret = rdecode(tokens, 200, &inflated[0], inflated.size());

			fprintf(stderr, "rdecode: %d\n", ret);

			rencoder output;

			// an RPC call is at least 5 tokens
			// list, ID, method, args, kwargs
			if (ret < 5) break;

			// each RPC call must be a list of the 4 items
			// it could also be multiple RPC calls wrapped
			// in a list.
			if (tokens[0].type() != type_list) break;

			if (tokens[1].type() == type_list)
			{
				int num_items = tokens->num_items();
				for (rtok_t* rpc = &tokens[1]; num_items; --num_items, rpc = skip_item(rpc))
				{
					incoming_rpc(rpc, &inflated[0], output);
					write_response(output, sock, ec);
					output.clear();
					if (ec) break;
				}
				if (ec) break;
			}
			else
			{
				incoming_rpc(&tokens[0], &inflated[0], output);
				write_response(output, sock, ec);
				if (ec) break;
			}

			// flush anything written to the SSL socket
			BIO* bio = SSL_get_wbio(sock->native_handle());
			TORRENT_ASSERT(bio);
			if (bio) BIO_flush(bio);

			buffer.erase(buffer.begin(), buffer.begin() + consumed_bytes);
			buffer_use -= consumed_bytes;
			fprintf(stderr, "consumed %d bytes (%d left)\n", consumed_bytes, buffer_use);
	
			// there's still data in the in-buffer that may be a message
			// don't get stuck in read if we have more messages to parse
			if (buffer_use > 0) goto parse_message;
	
			if (buffer.size() < 2048) buffer.resize(2048);

		} while (!m_shutdown);

		fprintf(stderr, "closing connection\n");
		sock->shutdown(ec);
		sock->lowest_layer().close(ec);
		delete sock;
	}

}

void deluge::write_response(rencoder const& output, ssl_socket* sock, error_code& ec)
{
	// ----
	rtok_t tmp[200];
	int r = rdecode(tmp, 200, output.data(), output.len());
	TORRENT_ASSERT(r > 0);
	printf("==> ");
	print_rtok(tmp, output.data());
	printf("\n");
	// ----

	z_stream strm;
	memset(&strm, 0, sizeof(strm));
	int ret = deflateInit(&strm, 9);
	if (ret != Z_OK) return;

	std::vector<char> deflated(output.len() * 3);
	strm.next_in = (Bytef*)output.data();
	strm.avail_in = output.len();
	strm.next_out = (Bytef*)&deflated[0];
	strm.avail_out = deflated.size();

	ret = deflate(&strm, Z_FINISH);

	deflated.resize(deflated.size() - strm.avail_out);
	deflateEnd(&strm);
	if (ret != Z_STREAM_END) return;

	ret = asio::write(*sock, asio::buffer(&deflated[0], deflated.size()), ec);
	if (ec)
	{
		fprintf(stderr, "write: %s\n", ec.message().c_str());
		return;
	}
	fprintf(stderr, "wrote %d bytes\n", ret);
}

void deluge::start(int port)
{
	if (m_accept_thread)
		stop();

	m_accept_thread = new thread(boost::bind(&deluge::accept_thread, this, port));
}

void deluge::do_stop()
{
	mutex::scoped_lock l(m_mutex);
	m_shutdown = true;
	m_cond.signal_all(l);
	if (m_listen_socket)
	{
		m_listen_socket->close();
		m_listen_socket = NULL;
	}
}

void deluge::stop()
{
	m_ios.post(boost::bind(&deluge::do_stop, this));

	m_accept_thread->join();
	delete m_accept_thread;
	m_accept_thread = NULL;
}

