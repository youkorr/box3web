#include "ftp_http_proxy.h"
#include "web.h"
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
  delayed_setup_ = true;
}

void FTPHTTPProxy::loop() {
  if (delayed_setup_) {
    static uint8_t startup_counter = 0;
    startup_counter++;
    
    if (startup_counter >= 5) {
      delayed_setup_ = false;
      this->setup_http_server();
    }
    return;
  }

  int64_t now = esp_timer_get_time() / 1000000;
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

void FTPHTTPProxy::file_transfer_task(void* param) {
  FileTransferContext* ctx = (FileTransferContext*)param;
  if (!ctx) {
    ESP_LOGE(TAG, "Contexte de transfert invalide");
    vTaskDelete(NULL);
    return;
  }
  
  ESP_LOGI(TAG, "Démarrage du transfert pour %s", ctx->remote_path.c_str());
  
  FTPHTTPProxy proxy_instance;
  int ftp_sock = -1;
  int data_sock = -1;
  bool success = false;
  int bytes_received = 0;

  const int buffer_size = 8192;
  char* buffer = (char*)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buffer) {
    buffer = (char*)malloc(buffer_size);
    if (!buffer) {
      ESP_LOGE(TAG, "Échec d'allocation pour le buffer");
      goto end_transfer;
    }
  }

  if (!proxy_instance.connect_to_ftp(ftp_sock, ctx->ftp_server.c_str(), ctx->username.c_str(), ctx->password.c_str())) {
    ESP_LOGE(TAG, "Échec de connexion FTP");
    goto end_transfer;
  }

  std::string extension;
  size_t dot_pos = ctx->remote_path.find_last_of('.');
  if (dot_pos != std::string::npos) {
    extension = ctx->remote_path.substr(dot_pos);
    std::transform(extension.begin(), extension.end(), extension.begin(), 
                  [](unsigned char c){ return std::tolower(c); });
  }

  if (extension == ".mp3") {
    httpd_resp_set_type(ctx->req, "audio/mpeg");
  } else if (extension == ".wav") {
    httpd_resp_set_type(ctx->req, "audio/wav");
  } else if (extension == ".ogg") {
    httpd_resp_set_type(ctx->req, "audio/ogg");
  } else if (extension == ".mp4") {
    httpd_resp_set_type(ctx->req, "video/mp4");
  } else if (extension == ".pdf") {
    httpd_resp_set_type(ctx->req, "application/pdf");
  } else if (extension == ".jpg" || extension == ".jpeg") {
    httpd_resp_set_type(ctx->req, "image/jpeg");
  } else if (extension == ".png") {
    httpd_resp_set_type(ctx->req, "image/png");
  } else {
    httpd_resp_set_type(ctx->req, "application/octet-stream");
    std::string filename = ctx->remote_path;
    size_t slash_pos = ctx->remote_path.find_last_of('/');
    if (slash_pos != std::string::npos) {
      filename = ctx->remote_path.substr(slash_pos + 1);
    }
    std::string header = "attachment; filename=\"" + filename + "\"";
    httpd_resp_set_hdr(ctx->req, "Content-Disposition", header.c_str());
  }

  send(ftp_sock, "PASV\r\n", 6, 0);
  bytes_received = recv(ftp_sock, buffer, buffer_size - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "227 ")) {
    ESP_LOGE(TAG, "Erreur en mode passif");
    goto end_transfer;
  }
  buffer[bytes_received] = '\0';

  char *pasv_start = strchr(buffer, '(');
  if (!pasv_start) {
    ESP_LOGE(TAG, "Format PASV incorrect");
    goto end_transfer;
  }
  
  int ip[4], port[2];
  sscanf(pasv_start, "(%d,%d,%d,%d,%d,%d)", &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]);
  int data_port = port[0] * 256 + port[1];
  
  data_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (data_sock < 0) {
    ESP_LOGE(TAG, "Échec de création du socket de données");
    goto end_transfer;
  }
  
  int flag = 1;
  setsockopt(data_sock, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));
  int rcvbuf = 32768;
  setsockopt(data_sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  struct timeval data_timeout = {.tv_sec = 10, .tv_usec = 0};
  setsockopt(data_sock, SOL_SOCKET, SO_RCVTIMEO, &data_timeout, sizeof(data_timeout));
  
  struct sockaddr_in data_addr;
  memset(&data_addr, 0, sizeof(data_addr));
  data_addr.sin_family = AF_INET;
  data_addr.sin_port = htons(data_port);
  data_addr.sin_addr.s_addr = htonl((ip[0] << 24) | (ip[1] << 16) | (ip[2] << 8) | ip[3]);
  
  if (connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) != 0) {
    ESP_LOGE(TAG, "Échec de connexion au port de données: %d", errno);
    goto end_transfer;
  }

  snprintf(buffer, buffer_size, "RETR %s\r\n", ctx->remote_path.c_str());
  send(ftp_sock, buffer, strlen(buffer), 0);

  bytes_received = recv(ftp_sock, buffer, buffer_size - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "150 ")) {
    ESP_LOGE(TAG, "Fichier non trouvé ou inaccessible");
    goto end_transfer;
  }
  
  ESP_LOGI(TAG, "Téléchargement du fichier %s démarré", ctx->remote_path.c_str());

  size_t total_bytes_transferred = 0;
  while (true) {
    bytes_received = recv(data_sock, buffer, buffer_size, 0);
    if (bytes_received <= 0) {
      if (bytes_received < 0) {
        ESP_LOGE(TAG, "Erreur de réception des données: %d", errno);
      }
      break;
    }
    
    total_bytes_transferred += bytes_received;
    esp_err_t err = httpd_resp_send_chunk(ctx->req, buffer, bytes_received);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Échec d'envoi au client: %d", err);
      goto end_transfer;
    }
    
    if (total_bytes_transferred % (1024 * 1024) == 0) {
      ESP_LOGI(TAG, "Transfert en cours: %zu MB", total_bytes_transferred / (1024 * 1024));
    }
    
    vTaskDelay(pdMS_TO_TICKS(1));
  }
  
  if (data_sock != -1) {
    close(data_sock);
    data_sock = -1;
  }
  
  bytes_received = recv(ftp_sock, buffer, buffer_size - 1, 0);
  if (bytes_received > 0 && strstr(buffer, "226 ")) {
    buffer[bytes_received] = '\0';
    ESP_LOGI(TAG, "Transfert terminé avec succès: %zu KB (%zu MB)", 
             total_bytes_transferred / 1024,
             total_bytes_transferred / (1024 * 1024));
    success = true;
  } else {
    ESP_LOGW(TAG, "Fin de transfert incomplète ou inattendue");
  }

end_transfer:
  if (buffer) {
    free(buffer);
  }
  
  if (data_sock != -1) close(data_sock);
  if (ftp_sock != -1) {
    send(ftp_sock, "QUIT\r\n", 6, 0);
    close(ftp_sock);
  }
  
  if (!success) {
    httpd_resp_send_err(ctx->req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erreur de transfert de fichier");
  } else {
    httpd_resp_send_chunk(ctx->req, NULL, 0);
  }
  
  delete ctx;
  vTaskDelete(NULL);
}

bool FTPHTTPProxy::list_ftp_directory(const std::string &remote_dir, httpd_req_t *req) {
  int ftp_sock = -1;
  int data_sock = -1;
  char buffer[1024];
  char *pasv_start = nullptr;
  int data_port = 0;
  int ip[4], port[2];
  int bytes_received;
  std::string file_list = "[";
  bool first_file = true;
  
  if (!connect_to_ftp(ftp_sock, ftp_server_.c_str(), username_.c_str(), password_.c_str())) {
    ESP_LOGE(TAG, "Échec de connexion FTP pour lister les fichiers");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "[]", 2);
    return false;
  }
  
  send(ftp_sock, "PASV\r\n", 6, 0);
  bytes_received = recv(ftp_sock, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "227 ")) {
    close(ftp_sock);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "[]", 2);
    return false;
  }
  buffer[bytes_received] = '\0';
  
  pasv_start = strchr(buffer, '(');
  if (!pasv_start) {
    close(ftp_sock);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "[]", 2);
    return false;
  }
  
  sscanf(pasv_start, "(%d,%d,%d,%d,%d,%d)", &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]);
  data_port = port[0] * 256 + port[1];
  
  data_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (data_sock < 0) {
    close(ftp_sock);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "[]", 2);
    return false;
  }
  
  struct sockaddr_in data_addr;
  memset(&data_addr, 0, sizeof(data_addr));
  data_addr.sin_family = AF_INET;
  data_addr.sin_port = htons(data_port);
  data_addr.sin_addr.s_addr = htonl((ip[0] << 24) | (ip[1] << 16) | (ip[2] << 8) | ip[3]);
  
  if (connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) != 0) {
    close(ftp_sock);
    close(data_sock);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "[]", 2);
    return false;
  }
  
  if (remote_dir.empty()) {
    send(ftp_sock, "LIST\r\n", 6, 0);
  } else {
    snprintf(buffer, sizeof(buffer), "LIST %s\r\n", remote_dir.c_str());
    send(ftp_sock, buffer, strlen(buffer), 0);
  }
  
  bytes_received = recv(ftp_sock, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0 || (!strstr(buffer, "150 ") && !strstr(buffer, "125 "))) {
    close(ftp_sock);
    close(data_sock);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "[]", 2);
    return false;
  }
  
  char entry_buffer[2048] = {0};
  
  while ((bytes_received = recv(data_sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
    buffer[bytes_received] = '\0';
    strcat(entry_buffer, buffer);
  }
  
  char *saveptr;
  char *line = strtok_r(entry_buffer, "\r\n", &saveptr);
  
  while (line) {
    char perms[11] = {0};
    char filename[256] = {0};
    unsigned long size = 0;
    
    if (sscanf(line, "%10s %*s %*s %*s %lu %*s %*s %*s %255s", perms, &size, filename) >= 2 ||
        sscanf(line, "%10s %*s %*s %*s %lu %*s %*s %255s", perms, &size, filename) >= 2) {
      
      if (strcmp(filename, ".") != 0 && strcmp(filename, "..") != 0) {
        bool is_dir = (perms[0] == 'd');
        bool known_file = false;
        bool is_shareable = false;
        
        for (const auto &file : ftp_files_) {
          if (file.path == filename) {
            known_file = true;
            is_shareable = file.shareable;
            break;
          }
        }
        
        if (!known_file) {
          FileEntry entry;
          entry.path = filename;
          entry.shareable = false;
          ftp_files_.push_back(entry);
        }
        
        if (!first_file) file_list += ",";
        first_file = false;
        
        file_list += "{\"name\":\"" + std::string(filename) + "\",";
        file_list += "\"path\":\"" + std::string(filename) + "\",";
        file_list += "\"type\":\"" + std::string(is_dir ? "directory" : "file") + "\",";
        file_list += "\"size\":" + std::to_string(size) + ",";
        file_list += "\"shareable\":" + std::string(is_shareable ? "true" : "false") + "}";
      }
    }
    
    line = strtok_r(NULL, "\r\n", &saveptr);
  }
  
  file_list += "]";
  
  close(data_sock);
  
  bytes_received = recv(ftp_sock, buffer, sizeof(buffer) - 1, 0);
  
  send(ftp_sock, "QUIT\r\n", 6, 0);
  close(ftp_sock);
  
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, file_list.c_str(), file_list.length());
  
  return true;
}

esp_err_t FTPHTTPProxy::toggle_shareable_handler(httpd_req_t *req) {
  auto *proxy = (FTPHTTPProxy *)req->user_ctx;
  
  char content[256];
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Données JSON manquantes");
    return ESP_FAIL;
  }
  content[ret] = '\0';
  
  std::string path;
  bool shareable = false;
  
  char *token = strtok(content, "{},:\"");
  while (token) {
    if (strcmp(token, "path") == 0) {
      token = strtok(NULL, "{},:\"");
      if (token) path = token;
    } else if (strcmp(token, "shareable") == 0) {
      token = strtok(NULL, "{},:\"");
      if (token) shareable = (strcmp(token, "true") == 0);
    }
    token = strtok(NULL, "{},:\"");
  }
  
  if (path.empty()) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Chemin de fichier manquant");
    return ESP_FAIL;
  }
  
  bool found = false;
  for (auto &file : proxy->ftp_files_) {
    if (file.path == path) {
      file.shareable = shareable;
      found = true;
      break;
    }
  }
  
  if (!found) {
    FileEntry entry;
    entry.path = path;
    entry.shareable = shareable;
    proxy->ftp_files_.push_back(entry);
  }
  
  ESP_LOGI(TAG, "Fichier %s marqué comme %s", 
           path.c_str(), shareable ? "partageable" : "non partageable");
  
  httpd_resp_sendstr(req, shareable ? "Fichier partageable" : "Fichier non partageable");
  return ESP_OK;
}

esp_err_t FTPHTTPProxy::http_req_handler(httpd_req_t *req) {
  auto *proxy = (FTPHTTPProxy *)req->user_ctx;
  std::string requested_path = req->uri;

  if (!requested_path.empty() && requested_path[0] == '/') {
    requested_path.erase(0, 1);
  }

  ESP_LOGI(TAG, "Requête de téléchargement reçue: %s", requested_path.c_str());
  
  bool path_valid = false;
  
  if (requested_path.compare(0, 6, "share/") == 0) {
    std::string token = requested_path.substr(6);
    
    for (const auto &share : proxy->active_shares_) {
      if (share.token == token) {
        requested_path = share.path;
        path_valid = true;
        ESP_LOGI(TAG, "Accès via lien de partage: %s -> %s", token.c_str(), requested_path.c_str());
        break;
      }
    }
  } else {
    path_valid = true;
  }
  
  if (!path_valid) {
    ESP_LOGW(TAG, "Chemin non autorisé: %s", requested_path.c_str());
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Fichier non trouvé ou accès non autorisé");
    return ESP_FAIL;
  }

  FileTransferContext* ctx = new FileTransferContext;
  if (!ctx) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erreur mémoire");
    return ESP_FAIL;
  }
  
  ctx->remote_path = requested_path;
  ctx->req = req;
  ctx->ftp_server = proxy->ftp_server_;
  ctx->username = proxy->username_;
  ctx->password = proxy->password_;

  BaseType_t task_created = xTaskCreatePinnedToCore(
    file_transfer_task,
    "file_transfer",
    8192,
    ctx,
    tskIDLE_PRIORITY + 1,
    NULL,
    1
  );

  if (task_created != pdPASS) {
    ESP_LOGE(TAG, "Échec de création de la tâche de transfert");
    delete ctx;
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erreur serveur");
    return ESP_FAIL;
  }

  return ESP_OK;
}

esp_err_t FTPHTTPProxy::file_list_handler(httpd_req_t *req) {
  auto *proxy = (FTPHTTPProxy *)req->user_ctx;
  
  std::string dir_path = "";
  char *query = NULL;
  size_t query_len = httpd_req_get_url_query_len(req) + 1;
  
  if (query_len > 1) {
    query = (char*)malloc(query_len);
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

esp_err_t FTPHTTPProxy::share_create_handler(httpd_req_t *req) {
  auto *proxy = (FTPHTTPProxy *)req->user_ctx;
  
  char content[256];
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Données JSON manquantes");
    return ESP_FAIL;
  }
  content[ret] = '\0';
  
  std::string path;
  int expiry = 24;
  
  char *token = strtok(content, "{},:\"");
  while (token) {
    if (strcmp(token, "path") == 0) {
      token = strtok(NULL, "{},:\"");
      if (token) path = token;
    } else if (strcmp(token, "expiry") == 0) {
      token = strtok(NULL, "{},:\"");
      if (token) expiry = atoi(token);
    }
    token = strtok(NULL, "{},:\"");
  }
  
  if (path.empty() || !proxy->is_shareable(path)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Fichier non partageable");
    return ESP_FAIL;
  }
  
  if (expiry < 1) expiry = 1;
  if (expiry > 72) expiry = 72;
  
  proxy->create_share_link(path, expiry);
  
  std::string token_str;
  for (const auto &share : proxy->active_shares_) {
    if (share.path == path) {
      token_str = share.token;
      break;
    }
  }
  
  char response[128];
  snprintf(response, sizeof(response), 
           "{\"link\": \"/share/%s\", \"expiry\": %d}",
           token_str.c_str(), expiry);
  
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, response, strlen(response));
  
  return ESP_OK;
}

esp_err_t FTPHTTPProxy::share_access_handler(httpd_req_t *req) {
  auto *proxy = (FTPHTTPProxy *)req->user_ctx;
  std::string requested_path = req->uri;
  
  if (requested_path.compare(0, 7, "/share/") == 0) {
    std::string token = requested_path.substr(7);
    
    for (const auto &share : proxy->active_shares_) {
      if (share.token == token) {
        return http_req_handler(req);
      }
    }
  }
  
  httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Lien de partage introuvable ou expiré");
  return ESP_FAIL;
}

esp_err_t FTPHTTPProxy::static_files_handler(httpd_req_t *req) {
  if (strcmp(req->uri, "/") == 0 || strcmp(req->uri, "/index.html") == 0) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, HTML_INDEX, strlen(HTML_INDEX));
    return ESP_OK;
  }
  
  httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Fichier non trouvé");
  return ESP_FAIL;
}

void FTPHTTPProxy::setup_http_server() {
  ESP_LOGI(TAG, "Démarrage du serveur HTTP...");

  wifi_ap_record_t ap_info;
  if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
    ESP_LOGW(TAG, "WiFi semble ne pas être connecté, mais on continue quand même");
  } else {
    ESP_LOGI(TAG, "WiFi connecté à %s", ap_info.ssid);
  }

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

  httpd_uri_t uri_static = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = static_files_handler,
    .user_ctx = this
  };
  httpd_register_uri_handler(server_, &uri_static);
  
  httpd_uri_t uri_files_api = {
    .uri = "/api/files",
    .method = HTTP_GET,
    .handler = file_list_handler,
    .user_ctx = this
  };
  httpd_register_uri_handler(server_, &uri_files_api);
  
  httpd_uri_t uri_toggle_shareable = {
    .uri = "/api/toggle-shareable",
    .method = HTTP_POST,
    .handler = toggle_shareable_handler,
    .user_ctx = this
  };
  httpd_register_uri_handler(server_, &uri_toggle_shareable);
  
  httpd_uri_t uri_share_api = {
    .uri = "/api/share",
    .method = HTTP_POST,
    .handler = share_create_handler,
    .user_ctx = this
  };
  httpd_register_uri_handler(server_, &uri_share_api);
  
  httpd_uri_t uri_share_access = {
    .uri = "/share/*",
    .method = HTTP_GET,
    .handler = share_access_handler,
    .user_ctx = this
  };
  httpd_register_uri_handler(server_, &uri_share_access);
  
  httpd_uri_t uri_download = {
    .uri = "/*",
    .method = HTTP_GET,
    .handler = http_req_handler,
    .user_ctx = this
  };
  httpd_register_uri_handler(server_, &uri_download);

  ESP_LOGI(TAG, "Serveur HTTP démarré avec succès sur le port %d", local_port_);
  ESP_LOGI(TAG, "Interface utilisateur accessible à http://[ip-esp]:%d/", local_port_);
}

}  // namespace ftp_http_proxy
}  // namespace esphome
