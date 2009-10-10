/*
 *  Phusion Passenger - http://www.modrails.com/
 *  Copyright (c) 2008, 2009 Phusion
 *
 *  "Phusion Passenger" is a trademark of Hongli Lai & Ninh Bui.
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a copy
 *  of this software and associated documentation files (the "Software"), to deal
 *  in the Software without restriction, including without limitation the rights
 *  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *  copies of the Software, and to permit persons to whom the Software is
 *  furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 *  THE SOFTWARE.
 */
#ifndef _PASSENGER_SESSION_H_
#define _PASSENGER_SESSION_H_

#include <sys/types.h>
#include <unistd.h>
#include <cerrno>

#include <boost/shared_ptr.hpp>
#include <boost/function.hpp>
#include <oxt/backtrace.hpp>
#include <oxt/system_calls.hpp>

#include "MessageChannel.h"
#include "StaticString.h"
#include "Exceptions.h"

namespace Passenger {

using namespace boost;
using namespace oxt;
using namespace std;


/**
 * Represents a single request/response pair of an application process.
 *
 * Session is used to forward a single HTTP request to an application
 * process, and to read back the HTTP response. A Session object is to
 * be used in the following manner:
 *
 *  -# Serialize the HTTP request headers into a format as expected by
 *     sendHeaders(), then send that string by calling sendHeaders().
 *  -# In case of a POST of PUT request, send the HTTP request body by
 *     calling sendBodyBlock(), possibly multiple times.
 *  -# Shutdown the writer end of the session channel (shutdownWriter())
 *     since you're now done sending data.
 *  -# The HTTP response can now be read through the session channel (getStream()).
 *  -# When the HTTP response has been read, the session must be closed.
 *     This is done by destroying the Session object.
 *
 * A usage example is shown in Process::connect().
 *
 * Session is an abstract base class. Concrete implementations can be found in
 * StandardSession and ApplicationPool::Client::RemoteSession.
 *
 * Session is not guaranteed to be thread-safe.
 */
class Session {
private:
	string poolIdentifier;
	
public:
	/**
	 * Concrete classes might throw arbitrary exceptions.
	 */
	virtual ~Session() {}
	
	/**
	 * Send HTTP request headers to the application. The HTTP headers must be
	 * converted into CGI headers, and then encoded into a string that matches this grammar:
	 *
	   @verbatim
	   headers ::= header*
	   header ::= name NUL value NUL
	   name ::= notnull+
	   value ::= notnull+
	   notnull ::= "\x01" | "\x02" | "\x02" | ... | "\xFF"
	   NUL = "\x00"
	   @endverbatim
	 *
	 * This method should be the first one to be called during the lifetime of a Session
	 * object, otherwise strange things may happen.
	 *
	 * @param headers The HTTP request headers, converted into CGI headers and encoded as
	 *                a string, according to the description.
	 * @param size The size, in bytes, of <tt>headers</tt>.
	 * @pre headers != NULL
	 * @throws IOException The I/O channel has already been closed or discarded.
	 * @throws SystemException Something went wrong during writing.
	 * @throws boost::thread_interrupted
	 */
	virtual void sendHeaders(const char *headers, unsigned int size) {
		TRACE_POINT();
		int stream = getStream();
		if (stream == -1) {
			throw IOException("Cannot write headers to the request handler "
				"because the I/O stream has already been closed or discarded.");
		}
		try {
			MessageChannel(stream).writeScalar(headers, size);
		} catch (SystemException &e) {
			e.setBriefMessage("An error occured while writing headers "
				"to the request handler");
			throw;
		}
	}
	
	/**
	 * Convenience shortcut for sendHeaders(const char *, unsigned int)
	 * @param headers The headers
	 * @throws IOException The I/O channel has already been closed or discarded.
	 * @throws SystemException Something went wrong during writing.
	 * @throws boost::thread_interrupted
	 */
	virtual void sendHeaders(const StaticString &headers) {
		sendHeaders(headers.c_str(), headers.size());
	}
	
	/**
	 * Send a chunk of HTTP request body data to the application.
	 * You can call this method as many times as is required to transfer
	 * the entire HTTP request body.
	 *
	 * This method must only be called after a sendHeaders(), otherwise
	 * strange things may happen.
	 *
	 * @param block A block of HTTP request body data to send.
	 * @param size The size, in bytes, of <tt>block</tt>.
	 * @throws IOException The I/O channel has already been closed or discarded.
	 * @throws SystemException Something went wrong during writing.
	 * @throws boost::thread_interrupted
	 */
	virtual void sendBodyBlock(const char *block, unsigned int size) {
		TRACE_POINT();
		int stream = getStream();
		if (stream == -1) {
			throw IOException("Cannot write request body block to the "
				"request handler because the I/O channel has "
				"already been closed or discarded.");
		}
		try {
			MessageChannel(stream).writeRaw(block, size);
		} catch (SystemException &e) {
			e.setBriefMessage("An error occured while sending the "
				"request body to the request handler");
			throw;
		}
	}
	
	/**
	 * Returns this session's channel's file descriptor. This stream is
	 * full-duplex, and will be automatically closed upon Session's
	 * destruction, unless discardStream() is called.
	 *
	 * @returns The file descriptor, or -1 if the I/O channel has already
	 *          been closed or discarded.
	 */
	virtual int getStream() const = 0;
	
	/**
	 * Set the timeout value for reading data from the I/O channel.
	 * If no data can be read within the timeout period, then the
	 * read call will fail with error EAGAIN or EWOULDBLOCK.
	 *
	 * @pre The I/O channel hasn't been closed or discarded.
	 * @param msec The timeout, in milliseconds. If 0 is given,
	 *             there will be no timeout.
	 * @throws SystemException Cannot set the timeout.
	 */
	virtual void setReaderTimeout(unsigned int msec) = 0;
	
	/**
	 * Set the timeout value for writing data from the I/O channel.
	 * If no data can be written within the timeout period, then the
	 * write call will fail with error EAGAIN or EWOULDBLOCK.
	 *
	 * @pre The I/O channel hasn't been closed or discarded.
	 * @param msec The timeout, in milliseconds. If 0 is given,
	 *             there will be no timeout.
	 * @throws SystemException Cannot set the timeout.
	 */
	virtual void setWriterTimeout(unsigned int msec) = 0;
	
	/**
	 * Indicate that we don't want to read data anymore from the I/O channel.
	 * Calling this method after closeStream()/discardStream() is called will
	 * have no effect.
	 *
	 * @throws SystemException Something went wrong.
	 * @throws boost::thread_interrupted
	 */
	virtual void shutdownReader() = 0;
	
	/**
	 * Indicate that we don't want to write data anymore to the I/O channel.
	 * Calling this method after closeStream()/discardStream() is called will
	 * have no effect.
	 *
	 * @throws SystemException Something went wrong.
	 * @throws boost::thread_interrupted
	 */
	virtual void shutdownWriter() = 0;
	
	/**
	 * Close the I/O stream.
	 *
	 * @throws SystemException Something went wrong.
	 * @throws boost::thread_interrupted
	 * @post getStream() == -1
	 */
	virtual void closeStream() = 0;
	
	/**
	 * Discard the I/O channel's file descriptor, so that the destructor
	 * won't automatically close it.
	 *
	 * @post getStream() == -1
	 */
	virtual void discardStream() = 0;
	
	/**
	 * Get the process ID of the application process that this session
	 * belongs to.
	 */
	virtual pid_t getPid() const = 0;
	
	virtual string getPoolIdentifier() const {
		return poolIdentifier;
	}
	
	virtual void setPoolIdentifier(const string &poolIdentifier) {
		this->poolIdentifier = poolIdentifier;
	}
};

typedef shared_ptr<Session> SessionPtr;


/**
 * A "standard" implementation of Session.
 */
class StandardSession: public Session {
protected:
	function<void()> closeCallback;
	int fd;
	pid_t pid;
	
public:
	StandardSession(pid_t pid,
	                const function<void()> &closeCallback,
	                int fd) {
		this->pid = pid;
		this->closeCallback = closeCallback;
		this->fd = fd;
	}

	virtual ~StandardSession() {
		TRACE_POINT();
		closeStream();
		closeCallback();
	}
	
	virtual int getStream() const {
		return fd;
	}
	
	virtual void setReaderTimeout(unsigned int msec) {
		MessageChannel(fd).setReadTimeout(msec);
	}
	
	virtual void setWriterTimeout(unsigned int msec) {
		MessageChannel(fd).setWriteTimeout(msec);
	}
	
	virtual void shutdownReader() {
		TRACE_POINT();
		if (fd != -1) {
			int ret = syscalls::shutdown(fd, SHUT_RD);
			if (ret == -1) {
				int e = errno;
				throw SystemException("Cannot shutdown the reader stream",
					e);
			}
		}
	}
	
	virtual void shutdownWriter() {
		TRACE_POINT();
		if (fd != -1) {
			int ret = syscalls::shutdown(fd, SHUT_WR);
			if (ret == -1) {
				throw SystemException("Cannot shutdown the writer stream",
					errno);
			}
		}
	}
	
	virtual void closeStream() {
		TRACE_POINT();
		if (fd != -1) {
			int ret = syscalls::close(fd);
			fd = -1;
			if (ret == -1) {
				if (errno == EIO) {
					throw SystemException("A write operation on the session stream failed",
						errno);
				} else {
					throw SystemException("Cannot close the session stream",
						errno);
				}
			}
		}
	}
	
	virtual void discardStream() {
		fd = -1;
	}
	
	virtual pid_t getPid() const {
		return pid;
	}
};


} // namespace Passenger

#endif /* _PASSENGER_SESSION_H_ */