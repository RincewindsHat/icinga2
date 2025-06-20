/* Icinga 2 | (c) 2012 Icinga GmbH | GPLv2+ */

#include "remote/httpserverconnection.hpp"
#include "remote/httphandler.hpp"
#include "remote/httputility.hpp"
#include "remote/apilistener.hpp"
#include "remote/apifunction.hpp"
#include "remote/jsonrpc.hpp"
#include "base/application.hpp"
#include "base/base64.hpp"
#include "base/convert.hpp"
#include "base/configtype.hpp"
#include "base/defer.hpp"
#include "base/exception.hpp"
#include "base/io-engine.hpp"
#include "base/logger.hpp"
#include "base/objectlock.hpp"
#include "base/timer.hpp"
#include "base/tlsstream.hpp"
#include "base/utility.hpp"
#include <chrono>
#include <limits>
#include <memory>
#include <stdexcept>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>
#include <boost/thread/once.hpp>

using namespace icinga;

auto const l_ServerHeader ("Icinga/" + Application::GetAppVersion());

HttpServerConnection::HttpServerConnection(const WaitGroup::Ptr& waitGroup, const String& identity, bool authenticated, const Shared<AsioTlsStream>::Ptr& stream)
	: HttpServerConnection(waitGroup, identity, authenticated, stream, IoEngine::Get().GetIoContext())
{
}

HttpServerConnection::HttpServerConnection(const WaitGroup::Ptr& waitGroup, const String& identity, bool authenticated, const Shared<AsioTlsStream>::Ptr& stream, boost::asio::io_context& io)
	: m_WaitGroup(waitGroup), m_Stream(stream), m_Seen(Utility::GetTime()), m_IoStrand(io), m_ShuttingDown(false), m_HasStartedStreaming(false),
	m_CheckLivenessTimer(io)
{
	if (authenticated) {
		m_ApiUser = ApiUser::GetByClientCN(identity);
	}

	{
		std::ostringstream address;
		auto endpoint (stream->lowest_layer().remote_endpoint());

		address << '[' << endpoint.address() << "]:" << endpoint.port();

		m_PeerAddress = address.str();
	}
}

void HttpServerConnection::Start()
{
	namespace asio = boost::asio;

	HttpServerConnection::Ptr keepAlive (this);

	IoEngine::SpawnCoroutine(m_IoStrand, [this, keepAlive](asio::yield_context yc) { ProcessMessages(yc); });
	IoEngine::SpawnCoroutine(m_IoStrand, [this, keepAlive](asio::yield_context yc) { CheckLiveness(yc); });
}

/**
 * Tries to asynchronously shut down the SSL stream and underlying socket.
 *
 * It is important to note that this method should only be called from within a coroutine that uses `m_IoStrand`.
 *
 * @param yc boost::asio::yield_context The coroutine yield context which you are calling this method from.
 */
void HttpServerConnection::Disconnect(boost::asio::yield_context yc)
{
	namespace asio = boost::asio;

	if (m_ShuttingDown) {
		return;
	}

	m_ShuttingDown = true;

	Log(LogInformation, "HttpServerConnection")
		<< "HTTP client disconnected (from " << m_PeerAddress << ")";

	m_CheckLivenessTimer.cancel();

	m_Stream->GracefulDisconnect(m_IoStrand, yc);

	auto listener (ApiListener::GetInstance());

	if (listener) {
		listener->RemoveHttpClient(this);
	}
}

void HttpServerConnection::StartStreaming()
{
	namespace asio = boost::asio;

	m_HasStartedStreaming = true;

	HttpServerConnection::Ptr keepAlive (this);

	IoEngine::SpawnCoroutine(m_IoStrand, [this, keepAlive](asio::yield_context yc) {
		if (!m_ShuttingDown) {
			char buf[128];
			asio::mutable_buffer readBuf (buf, 128);
			boost::system::error_code ec;

			do {
				m_Stream->async_read_some(readBuf, yc[ec]);
			} while (!ec);

			Disconnect(yc);
		}
	});
}

bool HttpServerConnection::Disconnected()
{
	return m_ShuttingDown;
}

static inline
bool EnsureValidHeaders(
	AsioTlsStream& stream,
	boost::beast::flat_buffer& buf,
	boost::beast::http::parser<true, boost::beast::http::string_body>& parser,
	boost::beast::http::response<boost::beast::http::string_body>& response,
	bool& shuttingDown,
	boost::asio::yield_context& yc
)
{
	namespace http = boost::beast::http;

	if (shuttingDown)
		return false;

	bool httpError = false;
	String errorMsg;

	boost::system::error_code ec;

	http::async_read_header(stream, buf, parser, yc[ec]);

	if (ec) {
		if (ec == boost::asio::error::operation_aborted)
			return false;

		errorMsg = ec.message();
		httpError = true;
	} else {
		switch (parser.get().version()) {
		case 10:
		case 11:
			break;
		default:
			errorMsg = "Unsupported HTTP version";
		}
	}

	if (!errorMsg.IsEmpty() || httpError) {
		response.result(http::status::bad_request);

		if (!httpError && parser.get()[http::field::accept] == "application/json") {
			HttpUtility::SendJsonBody(response, nullptr, new Dictionary({
				{ "error", 400 },
				{ "status", String("Bad Request: ") + errorMsg }
			}));
		} else {
			response.set(http::field::content_type, "text/html");
			response.body() = String("<h1>Bad Request</h1><p><pre>") + errorMsg + "</pre></p>";
			response.content_length(response.body().size());
		}

		response.set(http::field::connection, "close");

		http::async_write(stream, response, yc);
		stream.async_flush(yc);

		return false;
	}

	return true;
}

static inline
void HandleExpect100(
	AsioTlsStream& stream,
	boost::beast::http::request<boost::beast::http::string_body>& request,
	boost::asio::yield_context& yc
)
{
	namespace http = boost::beast::http;

	if (request[http::field::expect] == "100-continue") {
		http::response<http::string_body> response;

		response.result(http::status::continue_);

		http::async_write(stream, response, yc);
		stream.async_flush(yc);
	}
}

static inline
bool HandleAccessControl(
	AsioTlsStream& stream,
	boost::beast::http::request<boost::beast::http::string_body>& request,
	boost::beast::http::response<boost::beast::http::string_body>& response,
	boost::asio::yield_context& yc
)
{
	namespace http = boost::beast::http;

	auto listener (ApiListener::GetInstance());

	if (listener) {
		auto headerAllowOrigin (listener->GetAccessControlAllowOrigin());

		if (headerAllowOrigin) {
			auto allowedOrigins (headerAllowOrigin->ToSet<String>());

			if (!allowedOrigins.empty()) {
				auto& origin (request[http::field::origin]);

				if (allowedOrigins.find(std::string(origin)) != allowedOrigins.end()) {
					response.set(http::field::access_control_allow_origin, origin);
				}

				response.set(http::field::access_control_allow_credentials, "true");

				if (request.method() == http::verb::options && !request[http::field::access_control_request_method].empty()) {
					response.result(http::status::ok);
					response.set(http::field::access_control_allow_methods, "GET, POST, PUT, DELETE");
					response.set(http::field::access_control_allow_headers, "Authorization, Content-Type, X-HTTP-Method-Override");
					response.body() = "Preflight OK";
					response.content_length(response.body().size());
					response.set(http::field::connection, "close");

					http::async_write(stream, response, yc);
					stream.async_flush(yc);

					return false;
				}
			}
		}
	}

	return true;
}

static inline
bool EnsureAcceptHeader(
	AsioTlsStream& stream,
	boost::beast::http::request<boost::beast::http::string_body>& request,
	boost::beast::http::response<boost::beast::http::string_body>& response,
	boost::asio::yield_context& yc
)
{
	namespace http = boost::beast::http;

	if (request.method() != http::verb::get && request[http::field::accept] != "application/json") {
		response.result(http::status::bad_request);
		response.set(http::field::content_type, "text/html");
		response.body() = "<h1>Accept header is missing or not set to 'application/json'.</h1>";
		response.content_length(response.body().size());
		response.set(http::field::connection, "close");

		http::async_write(stream, response, yc);
		stream.async_flush(yc);

		return false;
	}

	return true;
}

static inline
bool EnsureAuthenticatedUser(
	AsioTlsStream& stream,
	boost::beast::http::request<boost::beast::http::string_body>& request,
	ApiUser::Ptr& authenticatedUser,
	boost::beast::http::response<boost::beast::http::string_body>& response,
	boost::asio::yield_context& yc
)
{
	namespace http = boost::beast::http;

	if (!authenticatedUser) {
		Log(LogWarning, "HttpServerConnection")
			<< "Unauthorized request: " << request.method_string() << ' ' << request.target();

		response.result(http::status::unauthorized);
		response.set(http::field::www_authenticate, "Basic realm=\"Icinga 2\"");
		response.set(http::field::connection, "close");

		if (request[http::field::accept] == "application/json") {
			HttpUtility::SendJsonBody(response, nullptr, new Dictionary({
				{ "error", 401 },
				{ "status", "Unauthorized. Please check your user credentials." }
			}));
		} else {
			response.set(http::field::content_type, "text/html");
			response.body() = "<h1>Unauthorized. Please check your user credentials.</h1>";
			response.content_length(response.body().size());
		}

		http::async_write(stream, response, yc);
		stream.async_flush(yc);

		return false;
	}

	return true;
}

static inline
bool EnsureValidBody(
	AsioTlsStream& stream,
	boost::beast::flat_buffer& buf,
	boost::beast::http::parser<true, boost::beast::http::string_body>& parser,
	ApiUser::Ptr& authenticatedUser,
	boost::beast::http::response<boost::beast::http::string_body>& response,
	bool& shuttingDown,
	boost::asio::yield_context& yc
)
{
	namespace http = boost::beast::http;

	{
		size_t maxSize = 1024 * 1024;
		Array::Ptr permissions = authenticatedUser->GetPermissions();

		if (permissions) {
			ObjectLock olock(permissions);

			for (const Value& permissionInfo : permissions) {
				String permission;

				if (permissionInfo.IsObjectType<Dictionary>()) {
					permission = static_cast<Dictionary::Ptr>(permissionInfo)->Get("permission");
				} else {
					permission = permissionInfo;
				}

				static std::vector<std::pair<String, size_t>> specialContentLengthLimits {
					 { "config/modify", 512 * 1024 * 1024 }
				};

				for (const auto& limitInfo : specialContentLengthLimits) {
					if (limitInfo.second <= maxSize) {
						continue;
					}

					if (Utility::Match(permission, limitInfo.first)) {
						maxSize = limitInfo.second;
					}
				}
			}
		}

		parser.body_limit(maxSize);
	}

	if (shuttingDown)
		return false;

	boost::system::error_code ec;

	http::async_read(stream, buf, parser, yc[ec]);

	if (ec) {
		if (ec == boost::asio::error::operation_aborted)
			return false;

		/**
		 * Unfortunately there's no way to tell an HTTP protocol error
		 * from an error on a lower layer:
		 *
		 * <https://github.com/boostorg/beast/issues/643>
		 */

		response.result(http::status::bad_request);

		if (parser.get()[http::field::accept] == "application/json") {
			HttpUtility::SendJsonBody(response, nullptr, new Dictionary({
				{ "error", 400 },
				{ "status", String("Bad Request: ") + ec.message() }
			}));
		} else {
			response.set(http::field::content_type, "text/html");
			response.body() = String("<h1>Bad Request</h1><p><pre>") + ec.message() + "</pre></p>";
			response.content_length(response.body().size());
		}

		response.set(http::field::connection, "close");

		http::async_write(stream, response, yc);
		stream.async_flush(yc);

		return false;
	}

	return true;
}

static inline
bool ProcessRequest(
	AsioTlsStream& stream,
	boost::beast::http::request<boost::beast::http::string_body>& request,
	ApiUser::Ptr& authenticatedUser,
	boost::beast::http::response<boost::beast::http::string_body>& response,
	HttpServerConnection& server,
	bool& hasStartedStreaming,
	const WaitGroup::Ptr& waitGroup,
	std::chrono::steady_clock::duration& cpuBoundWorkTime,
	boost::asio::yield_context& yc
)
{
	namespace http = boost::beast::http;

	try {
		// Cache the elapsed time to acquire a CPU semaphore used to detect extremely heavy workloads.
		auto start (std::chrono::steady_clock::now());
		CpuBoundWork handlingRequest (yc);
		cpuBoundWorkTime = std::chrono::steady_clock::now() - start;

		HttpHandler::ProcessRequest(waitGroup, stream, authenticatedUser, request, response, yc, server);
	} catch (const std::exception& ex) {
		if (hasStartedStreaming) {
			return false;
		}

		auto sysErr (dynamic_cast<const boost::system::system_error*>(&ex));

		if (sysErr && sysErr->code() == boost::asio::error::operation_aborted) {
			throw;
		}

		http::response<http::string_body> response;

		HttpUtility::SendJsonError(response, nullptr, 500, "Unhandled exception" , DiagnosticInformation(ex));

		http::async_write(stream, response, yc);
		stream.async_flush(yc);

		return true;
	}

	if (hasStartedStreaming) {
		return false;
	}

	http::async_write(stream, response, yc);
	stream.async_flush(yc);

	return true;
}

void HttpServerConnection::ProcessMessages(boost::asio::yield_context yc)
{
	namespace beast = boost::beast;
	namespace http = beast::http;
	namespace ch = std::chrono;

	try {
		/* Do not reset the buffer in the state machine.
		 * EnsureValidHeaders already reads from the stream into the buffer,
		 * EnsureValidBody continues. ProcessRequest() actually handles the request
		 * and needs the full buffer.
		 */
		beast::flat_buffer buf;

		while (m_WaitGroup->IsLockable()) {
			m_Seen = Utility::GetTime();

			http::parser<true, http::string_body> parser;
			http::response<http::string_body> response;

			parser.header_limit(1024 * 1024);
			parser.body_limit(-1);

			response.set(http::field::server, l_ServerHeader);

			if (!EnsureValidHeaders(*m_Stream, buf, parser, response, m_ShuttingDown, yc)) {
				break;
			}

			m_Seen = Utility::GetTime();
			auto start (ch::steady_clock::now());

			auto& request (parser.get());

			{
				auto method (http::string_to_verb(request["X-Http-Method-Override"]));

				if (method != http::verb::unknown) {
					request.method(method);
				}
			}

			HandleExpect100(*m_Stream, request, yc);

			auto authenticatedUser (m_ApiUser);

			if (!authenticatedUser) {
				authenticatedUser = ApiUser::GetByAuthHeader(std::string(request[http::field::authorization]));
			}

			Log logMsg (LogInformation, "HttpServerConnection");

			logMsg << "Request " << request.method_string() << ' ' << request.target()
				<< " (from " << m_PeerAddress
				<< ", user: " << (authenticatedUser ? authenticatedUser->GetName() : "<unauthenticated>")
				<< ", agent: " << request[http::field::user_agent]; //operator[] - Returns the value for a field, or "" if it does not exist.

			ch::steady_clock::duration cpuBoundWorkTime(0);
			Defer addRespCode ([&response, start, &logMsg, &cpuBoundWorkTime]() {
				logMsg << ", status: " << response.result() << ")";
				if (cpuBoundWorkTime >= ch::seconds(1)) {
					logMsg << " waited " << ch::duration_cast<ch::milliseconds>(cpuBoundWorkTime).count() << "ms on semaphore and";
				}

				logMsg << " took total " << ch::duration_cast<ch::milliseconds>(ch::steady_clock::now() - start).count() << "ms.";
			});

			if (!HandleAccessControl(*m_Stream, request, response, yc)) {
				break;
			}

			if (!EnsureAcceptHeader(*m_Stream, request, response, yc)) {
				break;
			}

			if (!EnsureAuthenticatedUser(*m_Stream, request, authenticatedUser, response, yc)) {
				break;
			}

			if (!EnsureValidBody(*m_Stream, buf, parser, authenticatedUser, response, m_ShuttingDown, yc)) {
				break;
			}

			m_Seen = std::numeric_limits<decltype(m_Seen)>::max();

			if (!ProcessRequest(*m_Stream, request, authenticatedUser, response, *this, m_HasStartedStreaming, m_WaitGroup, cpuBoundWorkTime, yc)) {
				break;
			}

			if (request.version() != 11 || request[http::field::connection] == "close") {
				break;
			}
		}
	} catch (const std::exception& ex) {
		if (!m_ShuttingDown) {
			Log(LogWarning, "HttpServerConnection")
				<< "Exception while processing HTTP request from " << m_PeerAddress << ": " << ex.what();
		}
	}

	Disconnect(yc);
}

void HttpServerConnection::CheckLiveness(boost::asio::yield_context yc)
{
	boost::system::error_code ec;

	for (;;) {
		m_CheckLivenessTimer.expires_from_now(boost::posix_time::seconds(5));
		m_CheckLivenessTimer.async_wait(yc[ec]);

		if (m_ShuttingDown) {
			break;
		}

		if (m_Seen < Utility::GetTime() - 10) {
			Log(LogInformation, "HttpServerConnection")
				<<  "No messages for HTTP connection have been received in the last 10 seconds.";

			Disconnect(yc);
			break;
		}
	}
}
