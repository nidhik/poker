/*
 * Copyright 2008, 2009, Dominik Geyer
 *
 * This file is part of HoldingNuts.
 *
 * HoldingNuts is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HoldingNuts is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HoldingNuts.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *     Dominik Geyer <dominik.geyer@holdingnuts.net>
 */


#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>


#if !defined(PLATFORM_WINDOWS)
# include <signal.h>
#endif

#include <vector>

#include "Config.h"
#include "Platform.h"
#include "Debug.h"
#include "Logger.h"

#include "Network.h"
#include "SysAccess.h"
#include "ConfigParser.hpp"
#include "game.hpp"

#include <boost/asio/io_service.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <string>

#include <iostream>
#include <memory>
#include <utility>
#include <boost/asio.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/bind.hpp>

using namespace boost::asio;
using namespace boost::asio::ip;


using namespace std;

ConfigParser config;

//------------------------------------------------------------------------------------------------------------------------
typedef struct {
    string msg;
    socktype	sock;
}message;


typedef std::deque<message> message_queue;

//----------------------------------------------------------------------

typedef std::map<socktype, session_ptr> session_map;
typedef std::pair<socktype, session_ptr> socket_session_pair;

class MessageDispatcher : public Dispatcher,
public std::enable_shared_from_this<MessageDispatcher>
{
public:
    virtual int dispatch(socktype fd, string msg)
    {
        
        session_map::const_iterator pos = participants_.find(fd);
        if (pos == participants_.end()) {
            return 0;
        } else {
            session_ptr session =  pos->second;
            
            return session->deliver(fd, msg);
        }
   
    }
    
    
    
    bool registerSession(session_ptr participant, socktype sock, sockaddr_in *saddr) {
        participants_.insert(socket_session_pair(sock, participant));
        return client_add(this, sock, saddr);
    }
    
    bool unregisterSession(session_ptr participant, socktype sock) {
        participants_.erase(sock);
        return client_remove(sock);
    }
    
    int handleSession(socktype sock, char data_[1024], std::size_t bytes) {
        return client_handle(sock, data_, bytes);
    }
private:
    session_map participants_;
    
};

//------------------------------------------------------------------------------------------------------------------------

MessageDispatcher dispatcher_singelton;

//------------------------------------------------------------------------------------------------------------------------

class session:
    public ClientSession,
    public std::enable_shared_from_this<session>
{
public:
    session(tcp::socket socket)
    : socket_(std::move(socket))
    {
    }
    
    void start()
    {
        log_msg("clientsock", "socket fd %d", socket_.native_handle());
        sockaddr_in saddr;
        memset(&saddr, 0, sizeof(sockaddr_in));

        dispatcher_singelton.registerSession(shared_from_this(), socket_.native_handle(), &saddr);
        
        do_read();
    }
    
    virtual int deliver(socktype fd, string msg) {
        // add the msg to the queue
        message toWrite;
        memset(&toWrite, 0, sizeof(message));
        toWrite.sock = fd;
        toWrite.msg = msg;

        
        bool write_in_progress = !write_msgs_.empty();
        write_msgs_.push_back(toWrite);
        if (!write_in_progress)
        {
            do_write();
        }
       
        return (int) msg.size();
    }
    
private:
    void do_read()
    {
        auto self(shared_from_this());
        socket_.async_read_some(boost::asio::buffer(data_, max_length),
                                [this, self](boost::system::error_code ec, std::size_t length)
                                {
                                    int sender = socket_.native_handle();
                                    if (!ec)
                                    {
                                        
                                        int status = client_handle(sender, data_, length);
                                        if (status <= 0)
                                        {
                                            if (!status)
                                                errno = 0;
                                            log_msg("clientsock", "(%d) socket closed (%d: %s)", sender, errno, strerror(errno));
                                            
                                            dispatcher_singelton.unregisterSession(shared_from_this(), sender);
                                            
                                            
                                        }
                                        do_read();
                                    }
                                    else if ((boost::asio::error::eof == ec) ||
                                        (boost::asio::error::connection_reset == ec))
                                    {
                                        // handle the disconnect.
                                        log_msg("clientsock", "(%d) socket disconnected (%d: %s)", sender, 0, strerror(errno));
                                        
                                        dispatcher_singelton.unregisterSession(shared_from_this(), sender);
                                    }
                                });
    }
    
    void do_write()
    {
        auto self(shared_from_this());
       
        boost::asio::async_write(socket_,
                                 boost::asio::buffer(write_msgs_.front().msg.data(),
                                                     write_msgs_.front().msg.size()),
                                 [this, self](boost::system::error_code ec, std::size_t /*length*/)
                                 {
                                     if (!ec)
                                     {
                                         write_msgs_.pop_front();
                                         if (!write_msgs_.empty())
                                         {
                                             do_write();
                                         }
                                     }
                                     else
                                     {
                                         int sender = socket_.native_handle();
                                         log_msg("clientsock", "Could not write. (%d) socket disconnected (%d: %s)", sender, 0, strerror(errno));
                                         dispatcher_singelton.unregisterSession(shared_from_this(), sender);
                                     }
                                 });
    }
    
    tcp::socket socket_;
    enum { max_length = 1024 };
    char data_[max_length];
    message_queue write_msgs_;
};

//------------------------------------------------------------------------------------------------------------------------

class server
{
public:
    server(boost::asio::io_service& io_service, short port)
    : acceptor_(io_service, tcp::endpoint(tcp::v4(), port)),
    socket_(io_service)
    {
        do_accept();
    }
    
private:
    void do_accept()
    {
        acceptor_.async_accept(socket_,
                               [this](boost::system::error_code ec)
                               {
                                   if (!ec)
                                   {
                                       std::make_shared<session>(std::move(socket_))->start();
                                   }
                                   
                                   do_accept();
                                           
                               });
    }
    
    tcp::acceptor acceptor_;
    tcp::socket socket_;
    
};

//------------------------------------------------------------------------------------------------------------------------

bool config_load()
{
	// include defaults
	#include "server_variables.hpp"
	
	
	// create config-dir if it doesn't yet exist
	sys_mkdir(sys_config_path());
	
	
	char cfgfile[1024];
	snprintf(cfgfile, sizeof(cfgfile), "%s/server.cfg", sys_config_path());
	
	if (config.load(cfgfile))
		log_msg("config", "Loaded configuration from %s", cfgfile);
	else
	{
		if (config.save(cfgfile))
			log_msg("config", "Saved initial configuration to %s", cfgfile);
	}
	
	return true;
}



void scheduleHandleGame(const boost::system::error_code& /*e*/,
           boost::asio::deadline_timer* t)
{
    gameloop();
    t->expires_at(t->expires_at() + boost::posix_time::seconds(1));
    t->async_wait(boost::bind(scheduleHandleGame,
                              boost::asio::placeholders::error, t));
   
}

int main(int argc, char* argv[])
{
    
    // use config-directory set on command-line
    if (argc >= 3 && (argv[1][0] == '-' && argv[1][1] == 'c'))
    {
        const char *path = argv[2];
        
        sys_set_config_path(path);
        log_msg("config", "Using manual config-directory '%s'", path);
    }
    
    
    // load config
    config_load();
    config.print();
    
    
    // start logging
    filetype *fplog = NULL;
    if (config.getBool("log"))
    {
        char logfile[1024];
        snprintf(logfile, sizeof(logfile), "%s/server.log", sys_config_path());
        fplog = file_open(logfile, mode_write);
        
        // log destination
        log_set(stdout, fplog);
        
        // log timestamp
        if (config.getBool("log_timestamp"))
            log_use_timestamp(1);
    }
    
    gameloop();
    
 
   
    
    try
    {
        
        boost::asio::io_service io_service;
        
        server s(io_service, 40888);
        
        boost::asio::deadline_timer t(io_service, boost::posix_time::seconds(5));
        t.async_wait(boost::bind(scheduleHandleGame,
                                 boost::asio::placeholders::error, &t));
        
        io_service.run();
    }
    catch (std::exception& e)
    {
        std::cerr << "Exception: " << e.what() << "\n";
        // close log-file
        
    }
    
    if (fplog)
        file_close(fplog);
    return 0;
}


