#pragma once
#include <vector>
#include "connections.h"

namespace arqmamq {

class ArqmaMQ;

class Message {
public:
  ArqmaMQ& arqmamq;
  std::vector<std::string_view> data;
  ConnectionID conn;
  std::string reply_tag;
  Access access;
  std::string remote;

  Message(ArqmaMQ& amq, ConnectionID cid, Access access, std::string remote)
      : arqmamq{amq}, conn{std::move(cid)}, access{std::move(access)}, remote{std::move(remote)} {}

  Message(const Message&) = delete;
  Message& operator=(const Message&) = delete;

  template <typename... Args>
  void send_back(std::string_view command, Args&&... args);

  template <typename... Args>
  void send_reply(Args&&... args);

  template <typename ReplyCallback, typename... Args>
  void send_request(std::string_view command, ReplyCallback&& callback, Args&&... args);

  class DeferredSend {
  public:
    ArqmaMQ& arqmamq;
    ConnectionID conn;
    std::string reply_tag;

    explicit DeferredSend(Message& m) : arqmamq{m.arqmamq}, conn{m.conn}, reply_tag{m.reply_tag} {}

    template <typename... Args>
    void back(std::string_view command, Args&&... args) const;

    template <typename... Args>
    void reply(Args&&... args) const;

    template <typename ReplyCallback, typename... Args>
    void request(std::string_view command, ReplyCallback&& callback, Args&&... args) const;
  };

  DeferredSend send_later() { return DeferredSend{*this}; }
};

}
