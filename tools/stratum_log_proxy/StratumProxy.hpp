/*
 The MIT License (MIT)

 Copyright (c) [2016] [BTC.COM]

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 */
#pragma once

#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <err.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>

#include <iostream>
#include <utility>
#include <mutex>
#include <set>
#include <map>
#include <memory>
#include <atomic>

#include <glog/logging.h>
#include <libconfig.h++>

#include <event2/dns.h>
#include <openssl/ssl.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/listener.h>
#include <event2/event.h>
#include <event2/thread.h>
#include <event2/bufferevent_ssl.h>

#include "SSLUtils.h"
#include "StratumBase.hpp"
#include "StratumAnalyzer.hpp"

using namespace std;
using namespace libconfig;

class StratumProxy {
protected:
  struct Session {
    string ip_;
    uint16_t port_;
    StratumWorker worker_;
    PoolInfo poolInfo_;

    struct evdns_base *evdnsBase_ = nullptr;
    struct bufferevent *downSession_ = nullptr;
    struct bufferevent *upSession_ = nullptr;

    atomic<bool> downSessionConnected_;
    atomic<bool> upSessionConnected_;

    struct evbuffer *downBuffer_ = nullptr;
    struct evbuffer *upBuffer_ = nullptr;

    StratumProxy *server_ = nullptr;
    shared_ptr<StratumAnalyzer> analyzer_;

    Session(
        struct bufferevent *down, struct bufferevent *up, StratumProxy *server)
      : downSession_(down)
      , upSession_(up)
      , server_(server) {
      downSessionConnected_ = false;
      upSessionConnected_ = false;

      downBuffer_ = evbuffer_new();
      upBuffer_ = evbuffer_new();

      evdnsBase_ =
          evdns_base_new(server->base_, EVDNS_BASE_INITIALIZE_NAMESERVERS);
      if (evdnsBase_ == nullptr) {
        LOG(FATAL) << "DNS init failed";
      }

      analyzer_ = make_shared<StratumAnalyzer>();
      analyzer_->setOnSubmitLogin(bind(&Session::onSubmitLogin, this, placeholders::_1));

      LOG(INFO) << "session created";
    }

    ~Session() {
      evbuffer_free(downBuffer_);
      evbuffer_free(upBuffer_);

      if (downSession_) bufferevent_free(downSession_);
      if (upSession_) bufferevent_free(upSession_);

      LOG(INFO) << "session destroyed";
    }

    void onSubmitLogin(StratumWorker worker) {
      worker_ = worker;
      LOG(INFO) << "miner login, wallet: " << worker_.wallet_ << ", user: " << worker_.userName_
        << ", worker: " << worker_.workerName_ << ", pwd: " << worker_.password_;
      
      analyzer_->run();
    }

    static void downReadCallback(struct bufferevent *bev, void *data) {
      auto session = static_cast<Session *>(data);
      auto buffer = bufferevent_get_input(bev);

      string content(evbuffer_get_length(buffer), 0);
      evbuffer_copyout(buffer, (char *)content.data(), content.size());
      session->analyzer_->addUploadText(content);

#ifndef NDEBUG
      LOG(INFO) << "upload(" << content.size() << "): " << content;
#endif

      session->downSessionConnected_ = true;
      if (session->upSessionConnected_) {
        bufferevent_write_buffer(session->upSession_, session->upBuffer_);
        evbuffer_drain(
            session->upBuffer_, evbuffer_get_length(session->upBuffer_));

        bufferevent_write_buffer(session->upSession_, buffer);
      } else {
        session->analyzer_->runOnce();
      }

      evbuffer_drain(buffer, evbuffer_get_length(buffer));
    }

    static void upReadCallback(struct bufferevent *bev, void *data) {
      auto session = static_cast<Session *>(data);
      auto buffer = bufferevent_get_input(bev);

      string content(evbuffer_get_length(buffer), 0);
      evbuffer_copyout(buffer, (char *)content.data(), content.size());
      session->analyzer_->addDownloadText(content);

#ifndef NDEBUG
      LOG(INFO) << "download(" << content.size() << "): " << content;
#endif

      session->upSessionConnected_ = true;
      if (session->downSessionConnected_) {
        bufferevent_write_buffer(session->downSession_, session->downBuffer_);
        evbuffer_drain(
            session->downBuffer_, evbuffer_get_length(session->downBuffer_));

        bufferevent_write_buffer(session->downSession_, buffer);
      } else {
        evbuffer_add_buffer(session->downBuffer_, buffer);
      }
      
      evbuffer_drain(buffer, evbuffer_get_length(buffer));
    }

    static void
    downEventCallback(struct bufferevent *bev, short events, void *data) {
      auto session = static_cast<Session *>(data);

      if (events & BEV_EVENT_CONNECTED) {
        LOG(INFO) << "downSession connected";
        session->downSessionConnected_ = true;
        bufferevent_write_buffer(session->downSession_, session->downBuffer_);
        evbuffer_drain(
            session->downBuffer_, evbuffer_get_length(session->downBuffer_));
        return;
      }

      if (events & BEV_EVENT_EOF) {
        LOG(INFO) << "downSession socket closed";
      } else if (events & BEV_EVENT_ERROR) {
        LOG(INFO) << "downSession got an error on the socket: "
                  << evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR());
      } else if (events & BEV_EVENT_TIMEOUT) {
        LOG(INFO) << "downSession socket read/write timeout, events: "
                  << events;
      } else {
        LOG(WARNING) << "downSession unhandled socket events: " << events;
      }

      session->downSessionConnected_ = false;
      session->server_->removeSession(session);
    }

    static void
    upEventCallback(struct bufferevent *bev, short events, void *data) {
      auto session = static_cast<Session *>(data);

      if (events & BEV_EVENT_CONNECTED) {
        LOG(INFO) << "upSession connected";
        session->upSessionConnected_ = true;
        bufferevent_write_buffer(session->upSession_, session->upBuffer_);
        evbuffer_drain(
            session->upBuffer_, evbuffer_get_length(session->upBuffer_));
        return;
      }

      if (events & BEV_EVENT_EOF) {
        LOG(INFO) << "upSession socket closed";
      } else if (events & BEV_EVENT_ERROR) {
        LOG(INFO) << "upSession got an error on the socket: "
                  << evutil_socket_error_to_string(EVUTIL_SOCKET_ERROR());
      } else if (events & BEV_EVENT_TIMEOUT) {
        LOG(INFO) << "upSession socket read/write timeout, events: " << events;
      } else {
        LOG(WARNING) << "upSession unhandled socket events: " << events;
      }

      session->upSessionConnected_ = false;
      session->server_->removeSession(session);
    }
  };

  const Config &config_;

  bool enableTLS_ = false;
  SSL_CTX *sslCTX_ = nullptr;
  struct sockaddr_in sinListen_;
  struct event_base *base_ = nullptr;
  struct evconnlistener *listener_ = nullptr;
  map<string, PoolInfo> pools_;

  set<Session *> sessions_;
  mutex sessionsLock_;

public:
  StratumProxy(const Config &config)
    : config_(config) {
    config.lookupValue("proxy.enable_tls", enableTLS_);

    if (enableTLS_) {
      // try get SSL CTX (load SSL cert and key)
      // any error will abort the process
      sslCTX_ = get_server_SSL_CTX(
          config.lookup("proxy.tls_cert_file").c_str(),
          config.lookup("proxy.tls_key_file").c_str());
    }
  }

  bool setup() {
    // ------------------- TCP Listen -------------------

    // Enable multithreading and flag BEV_OPT_THREADSAFE.
    // Without it, bufferevent_socket_new() will return NULL with flag
    // BEV_OPT_THREADSAFE.
    evthread_use_pthreads();

    base_ = event_base_new();
    if (!base_) {
      LOG(ERROR) << "server: cannot create base";
      return false;
    }

    string listenIP = config_.lookup("proxy.listen_addr").c_str();
    uint16_t listenPort = (unsigned int)config_.lookup("proxy.listen_port");

    memset(&sinListen_, 0, sizeof(sinListen_));
    sinListen_.sin_family = AF_INET;
    sinListen_.sin_port = htons(listenPort);
    sinListen_.sin_addr.s_addr = htonl(INADDR_ANY);
    if (listenIP.empty() ||
        inet_pton(AF_INET, listenIP.c_str(), &sinListen_.sin_addr) == 0) {
      LOG(ERROR) << "invalid ip: " << listenIP;
      return false;
    }

    listener_ = evconnlistener_new_bind(
        base_,
        listenerCallback,
        (void *)this,
        LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_FREE,
        -1,
        (struct sockaddr *)&sinListen_,
        sizeof(sinListen_));
    if (!listener_) {
      LOG(ERROR) << "cannot create listener: " << listenIP << ":" << listenPort;
      return false;
    }

    // ------------------- TCP Connect -------------------
    const auto &pools = config_.lookup("pools");
    for (int i = 0; i < pools.getLength(); i++) {
      pools_[pools[i].lookup("name").operator string()] = {
        pools[i].lookup("enable_tls").operator bool(),
        pools[i].lookup("host").operator string(),
        (uint16_t)pools[i].lookup("port").operator unsigned int(),
        pools[i].lookup("user").operator string(),
        pools[i].lookup("pwd").operator string(),
        pools[i].lookup("worker").operator string()
      };
    }

  for (const auto &itr : pools_) {
    if (itr.second.host_.empty() || itr.second.port_ == 0) {
      LOG(ERROR) << "invalid pools addr/port: " << itr.second.host_ << ":"
                 << itr.second.port_;
      return false;
    }
  }

    return true;
  }

  void run() {
    LOG(INFO) << "proxy running";
    event_base_dispatch(base_);
  }

  void stop() {
    event_base_loopexit(base_, NULL);
    LOG(INFO) << "proxy stopped";
  }

  ~StratumProxy() {
    if (listener_ != nullptr) {
      evconnlistener_free(listener_);
    }
    if (base_ != nullptr) {
      event_base_free(base_);
    }
  }

  void removeSession(Session *session) {
    lock_guard<mutex> sl(sessionsLock_);
    sessions_.erase(session);
    delete session;
  }

  static void listenerCallback(
      struct evconnlistener *listener,
      evutil_socket_t socket,
      struct sockaddr *saddr,
      int socklen,
      void *data) {
    StratumProxy *server = static_cast<StratumProxy *>(data);
    struct event_base *base = server->base_;

    // ---------------------- downSession ----------------------
    struct bufferevent *downBev = nullptr;

    if (server->enableTLS_) {
      SSL *downSSL = SSL_new(server->sslCTX_);
      if (downSSL == nullptr) {
        LOG(ERROR) << "Error calling SSL_new!";
        server->stop();
        return;
      }

      downBev = bufferevent_openssl_socket_new(
          base,
          socket,
          downSSL,
          BUFFEREVENT_SSL_ACCEPTING,
          BEV_OPT_CLOSE_ON_FREE | BEV_OPT_THREADSAFE);
    } else {
      downBev = bufferevent_socket_new(
          base, socket, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_THREADSAFE);
    }

    // If it was NULL with flag BEV_OPT_THREADSAFE,
    // please call evthread_use_pthreads() before you call event_base_new().
    if (downBev == nullptr) {
      LOG(ERROR) << "Error constructing bufferevent! Maybe you forgot call "
                    "evthread_use_pthreads() before event_base_new().";
      server->stop();
      return;
    }

    // ---------------------- add session ----------------------
    auto session = new Session(downBev, nullptr, server);
    {
      lock_guard<mutex> sl(server->sessionsLock_);
      server->sessions_.insert(session);
    }

    // set callback functions
    bufferevent_setcb(
        downBev,
        Session::downReadCallback,
        nullptr,
        Session::downEventCallback,
        session);
    // By default, a newly created bufferevent has writing enabled.
    bufferevent_enable(downBev, EV_READ | EV_WRITE);
  }

  bool connectUpstream(Session *session, struct bufferevent *&upBev, const PoolInfo &serverInfo) {
    upBev = nullptr;

    if (serverInfo.enableTLS_) {
      SSL *upSSL = SSL_new(get_client_SSL_CTX_With_Cache());
      if (upSSL == nullptr) {
        LOG(ERROR) << "Error calling SSL_new!";
        return false;
      }

      upBev = bufferevent_openssl_socket_new(
          base_,
          -1,
          upSSL,
          BUFFEREVENT_SSL_CONNECTING,
          BEV_OPT_CLOSE_ON_FREE | BEV_OPT_THREADSAFE);
    } else {
      upBev = bufferevent_socket_new(
          base_, -1, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_THREADSAFE);
    }

    // If it was NULL with flag BEV_OPT_THREADSAFE,
    // please call evthread_use_pthreads() before you call event_base_new().
    if (upBev == nullptr) {
      LOG(ERROR) << "Error constructing bufferevent! Maybe you forgot call "
                    "evthread_use_pthreads() before event_base_new().";
      return false;
    }

    bufferevent_setcb(
        upBev,
        Session::upReadCallback,
        nullptr,
        Session::upEventCallback,
        session);
    bufferevent_enable(upBev, EV_READ | EV_WRITE);

    // connect
    int res = bufferevent_socket_connect_hostname(
        upBev,
        session->evdnsBase_,
        AF_INET,
        serverInfo.host_.c_str(),
        serverInfo.port_);
    if (res != 0) {
      LOG(WARNING) << "upSession connecting failed";
      return false;
    }

    return true;
  }
};