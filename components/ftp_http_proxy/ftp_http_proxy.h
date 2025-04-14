#pragma once

#include "esphome/core/component.h"
#include <esp_http_server.h>
#include <string>
#include <vector>

namespace esphome {
namespace ftp_http_proxy {

struct FileTransferContext {
  std::string remote_path;
  httpd_req_t* req;
  std::string ftp_server;
  std::string username;
  std::string password;
};

class FTPHTTPProxy : public Component {
 public:
  void set_ftp_server(const std::string &server) { ftp_server_ = server; }
  void set_username(const std::string &username) { username_ = username; }
  void set_password(const std::string &password) { password_ = password; }
  void set_local_port(int port) { local_port_ = port; }
  
  bool is_shareable(const std::string &path);
  void create_share_link(const std::string &path, int expiry_hours);
  
  void setup() override;
  void loop() override;
  void setup_http_server();

 protected:
  static esp_err_t http_req_handler(httpd_req_t *req);
  static esp_err_t file_list_handler(httpd_req_t *req);
  static esp_err_t share_create_handler(httpd_req_t *req);
  static esp_err_t share_access_handler(httpd_req_t *req);
  static esp_err_t static_files_handler(httpd_req_t *req);
  static esp_err_t toggle_shareable_handler(httpd_req_t *req);
  
  static void file_transfer_task(void* param);
  bool connect_to_ftp(int& sock, const char* server, const char* username, const char* password);
  bool list_ftp_directory(const std::string &remote_dir, httpd_req_t *req);

  std::string ftp_server_;
  std::string username_;
  std::string password_;
  int local_port_{8080};
  int sock_{-1};
  httpd_handle_t server_{nullptr};
  bool delayed_setup_{false};
  
  // Structure pour le partage de fichiers
  struct ShareLink {
    std::string path;
    std::string token;
    int64_t expiry;  // Timestamp d'expiration
  };
  
  struct FileEntry {
    std::string path;
    bool shareable;
  };
  
  // Stockage des fichiers et paramètres de partage en mémoire
  std::vector<FileEntry> ftp_files_;
  std::vector<ShareLink> active_shares_;
};

}  // namespace ftp_http_proxy
}  // namespace esphome


