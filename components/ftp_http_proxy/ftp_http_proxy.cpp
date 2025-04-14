#include "ftp_http_proxy.h"
#include "web.h" // Inclure le fichier web.h pour l'interface HTML
#include "esphome/core/log.h"
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <cstring>
#include <arpa/inet.h>
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <string>
#include "esp_timer.h"
#include "esp_check.h"
#include "esp_wifi.h"

#ifndef HTTPD_410_GONE
#define HTTPD_410_GONE ((httpd_err_code_t)410)
#endif

static const char *TAG = "ftp_proxy";

namespace esphome {
namespace ftp_http_proxy {

void FTPHTTPProxy::setup() {
  ESP_LOGI(TAG, "Initialisation du proxy FTP/HTTP avec ESP-IDF 5.1.5");
  delayed_setup_ = true; // Démarrage différé pour attendre que WiFi/LWIP soit prêt
}

void FTPHTTPProxy::loop() {
  if (delayed_setup_) {
    static uint8_t startup_counter = 0;
    startup_counter++;
    if (startup_counter >= 5) { // Attendre 5 cycles pour s'assurer que tout est prêt
      delayed_setup_ = false;
      this->setup_http_server();
    }
    return;
  }

  // Nettoyage des liens de partage expirés
  int64_t now = esp_timer_get_time() / 1000000; // Temps en secondes
  active_shares_.erase(
    std::remove_if(
      active_shares_.begin(), 
      active_shares_.end(),
      [now](const ShareLink& link) { return link.expiry < now; }
    ),
    active_shares_.end()
  );
}

bool FTPHTTPProxy::is_shareable(const std::string &path) {
  for (const auto &file : ftp_files_) {
    if (file.path == path) {
      return file.shareable;
    }
  }
  return false;
}

void FTPHTTPProxy::create_share_link(const std::string &path, int expiry_hours) {
  if (!is_shareable(path)) {
    ESP_LOGW(TAG, "Tentative de partage d'un fichier non partageable: %s", path.c_str());
    return;
  }
  uint32_t random_value = esp_random();
  char token[16];
  snprintf(token, sizeof(token), "%08x", random_value);
  ShareLink share;
  share.path = path;
  share.token = token;
  share.expiry = (esp_timer_get_time() / 1000000) + (expiry_hours * 3600);
  active_shares_.push_back(share);
  ESP_LOGI(TAG, "Lien de partage créé pour %s: token=%s, expire dans %d heures", 
           path.c_str(), token, expiry_hours);
}

bool FTPHTTPProxy::connect_to_ftp(int& sock, const char* server, const char* username, const char* password) {
  struct hostent *ftp_host = gethostbyname(server);
  if (!ftp_host) {
    ESP_LOGE(TAG, "Échec de la résolution DNS");
    return false;
  }
  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    ESP_LOGE(TAG, "Échec de création du socket : %d", errno);
    return false;
  }
  int flag = 1;
  setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));
  int rcvbuf = 16384;
  setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  struct timeval timeout = {.tv_sec = 10, .tv_usec = 0};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(21);
  server_addr.sin_addr.s_addr = *((unsigned long *)ftp_host->h_addr);
  if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
    ESP_LOGE(TAG, "Échec de connexion FTP : %d", errno);
    close(sock);
    sock = -1;
    return false;
  }
  char buffer[512];
  int bytes_received = recv(sock, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "220 ")) {
    ESP_LOGE(TAG, "Message de bienvenue FTP non reçu");
    close(sock);
    sock = -1;
    return false;
  }
  buffer[bytes_received] = '\0';
  snprintf(buffer, sizeof(buffer), "USER %s\r\n", username);
  send(sock, buffer, strlen(buffer), 0);
  bytes_received = recv(sock, buffer, sizeof(buffer) - 1, 0);
  buffer[bytes_received] = '\0';
  snprintf(buffer, sizeof(buffer), "PASS %s\r\n", password);
  send(sock, buffer, strlen(buffer), 0);
  bytes_received = recv(sock, buffer, sizeof(buffer) - 1, 0);
  buffer[bytes_received] = '\0';
  if (!strstr(buffer, "230 ")) {
    ESP_LOGE(TAG, "Authentification FTP échouée: %s", buffer);
    close(sock);
    sock = -1;
    return false;
  }
  send(sock, "TYPE I\r\n", 8, 0);
  bytes_received = recv(sock, buffer, sizeof(buffer) - 1, 0);
  buffer[bytes_received] = '\0';
  return true;
}

bool FTPHTTPProxy::list_ftp_directory(const std::string &dir_path, httpd_req_t *req) {
  int ftp_sock = -1;
  if (!connect_to_ftp(ftp_sock, ftp_server_.c_str(), username_.c_str(), password_.c_str())) {
    ESP_LOGE(TAG, "Échec de connexion au serveur FTP");
    return false;
  }

  std::string command = "LIST";
  if (!dir_path.empty()) {
    command += " " + dir_path;
  }
  command += "\r\n";

  send(ftp_sock, command.c_str(), command.length(), 0);

  char buffer[512];
  int bytes_received = recv(ftp_sock, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "150 ")) {
    ESP_LOGE(TAG, "Échec de récupération de la liste de fichiers: %s", buffer);
    close(ftp_sock);
    return false;
  }

  send(ftp_sock, "PASV\r\n", 6, 0);
  bytes_received = recv(ftp_sock, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "227 ")) {
    ESP_LOGE(TAG, "Erreur en mode passif: %s", buffer);
    close(ftp_sock);
    return false;
  }

  char *pasv_start = strchr(buffer, '(');
  if (!pasv_start) {
    ESP_LOGE(TAG, "Format PASV incorrect");
    close(ftp_sock);
    return false;
  }

  int ip[4], port[2];
  if (sscanf(pasv_start, "(%d,%d,%d,%d,%d,%d)", &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]) != 6) {
    ESP_LOGE(TAG, "Impossible de parser la réponse PASV");
    close(ftp_sock);
    return false;
  }

  int data_port = port[0] * 256 + port[1];
  int data_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (data_sock < 0) {
    ESP_LOGE(TAG, "Échec de création du socket de données");
    close(ftp_sock);
    return false;
  }

  struct sockaddr_in data_addr;
  memset(&data_addr, 0, sizeof(data_addr));
  data_addr.sin_family = AF_INET;
  data_addr.sin_port = htons(data_port);
  data_addr.sin_addr.s_addr = (ip[0] << 24) | (ip[1] << 16) | (ip[2] << 8) | ip[3];

  if (connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) != 0) {
    ESP_LOGE(TAG, "Échec de connexion au port de données");
    close(data_sock);
    close(ftp_sock);
    return false;
  }

  std::string file_list = "[";
  while ((bytes_received = recv(data_sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
    buffer[bytes_received] = '\0';
    char *line = strtok(buffer, "\n");
    while (line) {
      std::string name = line; // À adapter selon le format de LIST
      if (!file_list.empty() && file_list.back() != '[') {
        file_list += ",";
      }
      file_list += "{\"name\":\"" + name + "\",\"path\":\"" + dir_path + "/" + name + "\",\"type\":\"file\",\"shareable\":false}";
      line = strtok(NULL, "\n");
    }
  }

  file_list += "]";
  close(data_sock);

  bytes_received = recv(ftp_sock, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received > 0 && strstr(buffer, "226 ")) {
    ESP_LOGI(TAG, "Liste des fichiers récupérée avec succès");
  } else {
    ESP_LOGW(TAG, "Fin de transfert incomplète ou inattendue: %s", buffer);
  }

  close(ftp_sock);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, file_list.c_str(), file_list.length());
  return true;
}

esp_err_t FTPHTTPProxy::file_list_handler(httpd_req_t *req) {
  auto *proxy = (FTPHTTPProxy *)req->user_ctx;

  std::string dir_path = "";
  char *query = NULL;
  size_t query_len = httpd_req_get_url_query_len(req) + 1;

  if (query_len > 1) {
    query = (char *)malloc(query_len);
    if (httpd_req_get_url_query_str(req, query, query_len) == ESP_OK) {
      char param[32];
      if (httpd_query_key_value(query, "dir", param, sizeof(param)) == ESP_OK) {
        dir_path = param;
      }
    }
    free(query);
  }

  ESP_LOGI(TAG, "Requête de liste de fichiers pour le répertoire: %s",
           dir_path.empty() ? "racine" : dir_path.c_str());

  if (!proxy->list_ftp_directory(dir_path, req)) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Échec de la récupération de la liste de fichiers");
    return ESP_FAIL;
  }

  return ESP_OK;
}

esp_err_t FTPHTTPProxy::toggle_shareable_handler(httpd_req_t *req) {
  auto *proxy = (FTPHTTPProxy *)req->user_ctx;
  char buf[100];
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty request");
    return ESP_FAIL;
  }
  buf[ret] = '\0';
  std::string path = buf;
  bool found = false;
  for (auto &file : proxy->ftp_files_) {
    if (file.path == path) {
      file.shareable = !file.shareable;
      found = true;
      break;
    }
  }
  httpd_resp_set_type(req, "application/json");
  if (found) {
    httpd_resp_send(req, "{\"success\": true}", 16);
  } else {
    httpd_resp_send(req, "{\"success\": false}", 17);
  }
  return ESP_OK;
}

void FTPHTTPProxy::setup_http_server() {
  ESP_LOGI(TAG, "Démarrage du serveur HTTP...");
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = local_port_;
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.recv_wait_timeout = 30;
  config.send_wait_timeout = 30;
  config.max_uri_handlers = 8;
  config.max_resp_headers = 16;
  config.stack_size = 8192;
  config.lru_purge_enable = true;
  config.core_id = 0;

  esp_err_t ret = httpd_start(&server_, &config);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Échec du démarrage du serveur HTTP: %s", esp_err_to_name(ret));
    return;
  }

  const httpd_uri_t uri_static = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = static_files_handler,
    .user_ctx  = this
  };
  ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(server_, &uri_static));

  const httpd_uri_t uri_files_api = {
    .uri       = "/api/files",
    .method    = HTTP_GET,
    .handler   = file_list_handler,
    .user_ctx  = this
  };
  ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(server_, &uri_files_api));

  ESP_LOGI(TAG, "Serveur HTTP démarré avec succès sur le port %d", local_port_);
}

esp_err_t FTPHTTPProxy::static_files_handler(httpd_req_t *req) {
  if (strcmp(req->uri, "/") == 0 || strcmp(req->uri, "/index.html") == 0) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_INDEX, strlen(HTML_INDEX));
    return ESP_OK;
  }
  if (strcmp(req->uri, "/favicon.ico") == 0) {
    httpd_resp_set_type(req, "image/x-icon");
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, "", 0);
    return ESP_OK;
  }
  httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Fichier non trouvé");
  return ESP_FAIL;
}

}  // namespace ftp_http_proxy
}  // namespace esphome
