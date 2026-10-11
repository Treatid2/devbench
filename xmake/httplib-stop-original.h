inline void Server::stop() {
  if (is_running_) {
    assert(svr_sock_ != INVALID_SOCKET);
    std::atomic<socket_t> sock(svr_sock_.exchange(INVALID_SOCKET));
    detail::shutdown_socket(sock);
    detail::close_socket(sock);
  }
  is_decommisioned = false;
}
