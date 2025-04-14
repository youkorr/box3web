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

  // Ne pas essayer de réinitialiser le watchdog, utiliser celui déjà configuré
  // Planifier le démarrage du serveur HTTP après un délai pour que le WiFi et LWIP soient prêts
  delayed_setup_ = true;
}

void FTPHTTPProxy::loop() {
  // Premier passage dans loop: initialiser le serveur HTTP
  if (delayed_setup_) {
    static uint8_t startup_counter = 0;
    startup_counter++;
    
    // Attendre 5 passages dans loop pour s'assurer que tout est prêt
    if (startup_counter >= 5) {
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
  // Rechercher le fichier dans notre liste
  for (const auto &file : ftp_files_) {
    if (file.path == path) {
      return file.shareable;
    }
  }
  return false;
}

void FTPHTTPProxy::create_share_link(const std::string &path, int expiry_hours) {
  // Vérifier si le fichier est partageable
  if (!is_shareable(path)) {
    ESP_LOGW(TAG, "Tentative de partage d'un fichier non partageable: %s", path.c_str());
    return;
  }
  
  // Générer un token aléatoire
  uint32_t random_value = esp_random();
  char token[16];
  snprintf(token, sizeof(token), "%08x", random_value);
  
  // Créer le lien de partage avec expiration
  ShareLink share;
  share.path = path;
  share.token = token;
  share.expiry = (esp_timer_get_time() / 1000000) + (expiry_hours * 3600);
  
  // Ajouter à la liste des partages actifs
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

  // Configuration du socket pour être plus robuste
  int flag = 1;
  setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));
  
  // Augmenter la taille du buffer de réception
  int rcvbuf = 16384;
  setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

  // Timeout pour les opérations socket
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

  // Authentification
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

  // Mode binaire
  send(sock, "TYPE I\r\n", 8, 0);
  bytes_received = recv(sock, buffer, sizeof(buffer) - 1, 0);
  buffer[bytes_received] = '\0';

  return true;
}

/* Cette fonction exécute le transfert de fichier dans une tâche séparée */
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

  // Allocation du buffer avec PSRAM si disponible
  bool has_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0;
  const int buffer_size = 8192;  // Taille optimisée pour l'ESP32-S3
  char* buffer = nullptr;
  
  if (has_psram) {
    buffer = (char*)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "Utilisation de la PSRAM pour le buffer");
  } else {
    buffer = (char*)malloc(buffer_size);
    ESP_LOGI(TAG, "PSRAM non disponible, utilisation de la RAM standard");
  }
  
  if (!buffer) {
    ESP_LOGE(TAG, "Échec d'allocation pour le buffer");
    goto end_transfer;
  }

  // Connexion au serveur FTP
  if (!proxy_instance.connect_to_ftp(ftp_sock, ctx->ftp_server.c_str(), ctx->username.c_str(), ctx->password.c_str())) {
    ESP_LOGE(TAG, "Échec de connexion FTP");
    goto end_transfer;
  }

  // Configuration des headers HTTP
  {
    std::string extension = "";
    size_t dot_pos = ctx->remote_path.find_last_of('.');
    if (dot_pos != std::string::npos) {
      extension = ctx->remote_path.substr(dot_pos);
      std::transform(extension.begin(), extension.end(), extension.begin(), 
                     [](unsigned char c){ return std::tolower(c); });
    }

    // Configuration du type MIME
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
      // Type par défaut pour les fichiers inconnus
      httpd_resp_set_type(ctx->req, "application/octet-stream");
      
      // Extraire le nom du fichier pour Content-Disposition
      std::string filename = ctx->remote_path;
      size_t slash_pos = ctx->remote_path.find_last_of('/');
      if (slash_pos != std::string::npos) {
        filename = ctx->remote_path.substr(slash_pos + 1);
      }
      
      std::string header = "attachment; filename=\"" + filename + "\"";
      httpd_resp_set_hdr(ctx->req, "Content-Disposition", header.c_str());
    }
    
    // En-têtes pour permettre la mise en cache et les requêtes par plage
    httpd_resp_set_hdr(ctx->req, "Accept-Ranges", "bytes");
  }

  // Mode passif
  send(ftp_sock, "PASV\r\n", 6, 0);
  int bytes_received = recv(ftp_sock, buffer, buffer_size - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "227 ")) {
    ESP_LOGE(TAG, "Erreur en mode passif");
    goto end_transfer;
  }
  buffer[bytes_received] = '\0';

  // Analyse de la réponse PASV
  {
    char *pasv_start = strchr(buffer, '(');
    if (!pasv_start) {
      ESP_LOGE(TAG, "Format PASV incorrect");
      goto end_transfer;
    }
    
    int ip[4], port[2];
    sscanf(pasv_start, "(%d,%d,%d,%d,%d,%d)", &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]);
    int data_port = port[0] * 256 + port[1];
    
    // Connexion au port de données
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
  }

  // Envoyer la commande RETR pour récupérer le fichier
  snprintf(buffer, buffer_size, "RETR %s\r\n", ctx->remote_path.c_str());
  send(ftp_sock, buffer, strlen(buffer), 0);

  bytes_received = recv(ftp_sock, buffer, buffer_size - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "150 ")) {
    ESP_LOGE(TAG, "Fichier non trouvé ou inaccessible");
    goto end_transfer;
  }
  
  ESP_LOGI(TAG, "Téléchargement du fichier %s démarré", ctx->remote_path.c_str());

  // Boucle principale de transfert de données
  {
    size_t total_bytes_transferred = 0;
    
    while (true) {
      bytes_received = recv(data_sock, buffer, buffer_size, 0);
      if (bytes_received <= 0) {
        if (bytes_received < 0) {
          ESP_LOGE(TAG, "Erreur de réception des données: %d", errno);
        }
        break;
      }
      
      // Mise à jour du total transféré
      total_bytes_transferred += bytes_received;
      
      // Envoi du chunk au client HTTP
      esp_err_t err = httpd_resp_send_chunk(ctx->req, buffer, bytes_received);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Échec d'envoi au client: %d", err);
        goto end_transfer;
      }
      
      // Journalisation périodique pour suivre la progression
      if (total_bytes_transferred % (1024 * 1024) == 0) { // Log tous les 1MB
        ESP_LOGI(TAG, "Transfert en cours: %zu MB", total_bytes_transferred / (1024 * 1024));
      }
      
      // Yield pour permettre à d'autres tâches de s'exécuter
      vTaskDelay(pdMS_TO_TICKS(1));
    }
    
    // Vérifier que le transfert s'est bien terminé
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
  }

end_transfer:
  // Nettoyage des ressources
  if (buffer) {
    if (has_psram) {
      heap_caps_free(buffer);
    } else {
      free(buffer);
    }
  }
  
  if (data_sock != -1) close(data_sock);
  if (ftp_sock != -1) {
    send(ftp_sock, "QUIT\r\n", 6, 0);
    close(ftp_sock);
  }
  
  // Terminer la réponse HTTP
  if (!success) {
    httpd_resp_send_err(ctx->req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erreur de transfert de fichier");
  } else {
    httpd_resp_send_chunk(ctx->req, NULL, 0);
  }
  
  // Libérer la mémoire du contexte
  delete ctx;
  
  // Supprimer cette tâche
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
  
  // Mode passif
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
  
  // Lister les fichiers du répertoire
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
  
  // Construction de la réponse JSON
  char entry_buffer[2048] = {0};
  
  while ((bytes_received = recv(data_sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
    buffer[bytes_received] = '\0';
    strcat(entry_buffer, buffer);
  }
  
  // Traiter les entrées
  char *saveptr;
  char *line = strtok_r(entry_buffer, "\r\n", &saveptr);
  
  // Construction de la liste de fichiers
  while (line) {
    // Analyser le listing FTP (format typique: "drwxr-xr-x 1 owner group size date filename")
    char perms[11] = {0};
    char filename[256] = {0};
    unsigned long size = 0;
    
    // L'analyse est simplifiée - dans un code réel, on ferait une analyse plus robuste
    // Le format peut varier selon les serveurs FTP
    if (sscanf(line, "%10s %*s %*s %*s %lu %*s %*s %*s %255s", perms, &size, filename) >= 2 ||
        sscanf(line, "%10s %*s %*s %*s %lu %*s %*s %255s", perms, &size, filename) >= 2) {
      
      // Ignorer "." et ".."
      if (strcmp(filename, ".") != 0 && strcmp(filename, "..") != 0) {
        bool is_dir = (perms[0] == 'd');
        
        // Vérifier si on connaît déjà ce fichier
        bool known_file = false;
        bool is_shareable = false;
        
        for (const auto &file : ftp_files_) {
          if (file.path == filename) {
            known_file = true;
            is_shareable = file.shareable;
            break;
          }
        }
        
        // Si c'est un nouveau fichier, l'ajouter à notre liste
        if (!known_file) {
          FileEntry entry;
          entry.path = filename;
          entry.shareable = false; // Non partageable par défaut
          ftp_files_.push_back(entry);
        }
        
        if (!first_file) file_list += ",";
        first_file = false;
        
        // Ajouter l'entrée à la liste JSON
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
  
  // Fermer les sockets
  close(data_sock);
  
  bytes_received = recv(ftp_sock, buffer, sizeof(buffer) - 1, 0);
  
  send(ftp_sock, "QUIT\r\n", 6, 0);
  close(ftp_sock);
  
  // Envoyer la réponse JSON
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, file_list.c_str(), file_list.length());
  
  return true;
}

esp_err_t FTPHTTPProxy::toggle_shareable_handler(httpd_req_t *req) {
  auto *proxy = (FTPHTTPProxy *)req->user_ctx;
  
  // Lire le corps de la requête JSON
  char content[256];
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Données JSON manquantes");
    return ESP_FAIL;
  }
  content[ret] = '\0';
  
  // Analyser le JSON (implémentation basique)
  // Format attendu: {"path": "chemin/du/fichier", "shareable": true|false}
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
  
  // Mettre à jour notre liste de fichiers
  bool found = false;
  for (auto &file : proxy->ftp_files_) {
    if (file.path == path) {
      file.shareable = shareable;
      found = true;
      break;
    }
  }
  
  if (!found) {
    // Ajouter un nouveau fichier
    FileEntry entry;
    entry.path = path;
    entry.shareable = shareable;
    proxy->ftp_files_.push_back(entry);
  }
  
  ESP_LOGI(TAG, "Fichier %s marqué comme %s", 
           path.c_str(), shareable ? "partageable" : "non partageable");
  
  // Réponse simple
  httpd_resp_sendstr(req, shareable ? "Fichier partageable" : "Fichier non partageable");
  return ESP_OK;
}

esp_err_t FTPHTTPProxy::http_req_handler(httpd_req_t *req) {
  auto *proxy = (FTPHTTPProxy *)req->user_ctx;
  std::string requested_path = req->uri;

  // Suppression du premier slash
  if (!requested_path.empty() && requested_path[0] == '/') {
    requested_path.erase(0, 1);
  }

  ESP_LOGI(TAG, "Requête de téléchargement reçue: %s", requested_path.c_str());
  
  // Vérifier si c'est un lien de partage valide
  bool path_valid = false;
  
  // Format typique: /share/TOKEN
  if (requested_path.compare(0, 6, "share/") == 0) {
    std::string token = requested_path.substr(6);
    
    // Chercher le token dans les partages actifs
    for (const auto &share : proxy->active_shares_) {
      if (share.token == token) {
        requested_path = share.path;
        path_valid = true;
        ESP_LOGI(TAG, "Accès via lien de partage: %s -> %s", token.c_str(), requested_path.c_str());
        break;
      }
    }
  } else {
    // Vérifier si le fichier est dans la liste des fichiers connus
    // Tous les fichiers connus sont accessibles directement (pas besoin d'une liste prédéfinie)
    path_valid = true;
  }
  
  if (!path_valid) {
    ESP_LOGW(TAG, "Chemin non autorisé: %s", requested_path.c_str());
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Fichier non trouvé ou accès non autorisé");
    return ESP_FAIL;
  }

  // Créer le contexte de transfert
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

  // Créer une tâche dédiée pour le transfert de fichier pour éviter le blocage
  BaseType_t task_created = xTaskCreatePinnedToCore(
    file_transfer_task,           // Fonction de tâche
    "file_transfer",              // Nom de tâche
    8192,                         // Taille de la pile
    ctx,                          // Paramètres de la tâche
    tskIDLE_PRIORITY + 1,         // Priorité
    NULL,                         // Handle (non nécessaire)
    1                             // S'exécute sur le cœur 1 (laisse le cœur 0 pour l'interface WiFi)
  );

  if (task_created != pdPASS) {
    ESP_LOGE(TAG, "Échec de création de la tâche de transfert");
    delete ctx;
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erreur serveur");
    return ESP_FAIL;
  }

  // La tâche dédiée va gérer le transfert et la réponse HTTP
  return ESP_OK;
}

esp_err_t FTPHTTPProxy::file_list_handler(httpd_req_t *req) {
  auto *proxy = (FTPHTTPProxy *)req->user_ctx;
  
  // Extraire le chemin du répertoire depuis la requête (éventuellement)
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
  
  // Lire le corps de la requête JSON
  char content[256];
  int ret = httpd_req_recv(req, content, sizeof(content) - 1);
  if (ret <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Données JSON manquantes");
    return ESP_FAIL;
  }
  content[ret] = '\0';
  
  // Analyser le JSON (implémentation basique)
  // Format attendu: {"path": "chemin/du/fichier", "expiry": 24}
  std::string path;
  int expiry = 24;  // Par défaut 24h
  
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
  
  // Vérifier si le chemin est valide et partageable
  if (path.empty() || !proxy->is_shareable(path)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Fichier non partageable");
    return ESP_FAIL;
  }
  
  // Limiter l'expiration à une plage raisonnable (1h - 72h)
  if (expiry < 1) expiry = 1;
  if (expiry > 72) expiry = 72;
  
  // Créer le lien de partage
  proxy->create_share_link(path, expiry);
  
  // Trouver le token du lien qu'on vient de créer
  std::string token_str;
  for (const auto &share : proxy->active_shares_) {
    if (share.path == path) {
      token_str = share.token;
      break;
    }
  }
  
  // Réponse avec le lien créé
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
  
  // Format: /share/TOKEN
  if (requested_path.compare(0, 7, "/share/") == 0) {
    std::string token = requested_path.substr(7);
    
    // Chercher le token dans les partages actifs
    for (const auto &share : proxy->active_shares_) {
      if (share.token == token) {
        // Redirige vers le gestionnaire de téléchargement normal
        // en utilisant le chemin effectif
        return http_req_handler(req);
      }
    }
  }
  
  httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Lien de partage introuvable ou expiré");
  return ESP_FAIL;
}

esp_err_t FTPHTTPProxy::static_files_handler(httpd_req_t *req) {
  // Interface principale
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

  // Vérification de la connexion réseau
  wifi_ap_record_t ap_info;
  if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
    ESP_LOGW(TAG, "WiFi semble ne pas être connecté, mais on continue quand même");
  } else {
    ESP_LOGI(TAG, "WiFi connecté à %s", ap_info.ssid);
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = local_port_;
  config.uri_match_fn = httpd_uri_match_wildcard;
  
  // Optimisations pour ESP-IDF 5.1.5
  config.recv_wait_timeout = 30;    // 30 secondes
  config.send_wait_timeout = 30;    // 30 secondes
  config.max_uri_handlers = 8;        
  config.max_resp_headers = 16;
  config.stack_size = 8192;         // Taille de pile suffisante
  config.lru_purge_enable = true;   // Activer la purge LRU
  config.core_id = 0;               // S'exécute sur le cœur 0
  
  esp_err_t ret = httpd_start(&server_, &config);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Échec du démarrage du serveur HTTP: %s", esp_err_to_name(ret));
    return;
  }

  // Enregistrement des gestionnaires d'URI
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
  
  const httpd_uri_t uri_toggle_shareable = {
    .uri       = "/api/toggle-shareable",
    .method    = HTTP_POST,
    .handler   = toggle_shareable_handler,
    .user_ctx  = this
  };
  ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(server_, &uri_toggle_shareable));
  
  const httpd_uri_t uri_share_api = {
    .uri       = "/api/share",
    .method    = HTTP_POST,
    .handler   = share_create_handler,
    .user_ctx  = this
  };
  ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(server_, &uri_share_api));
  
  const httpd_uri_t uri_share_access = {
    .uri       = "/share/*",
    .method    = HTTP_GET,
    .handler   = share_access_handler,
    .user_ctx  = this
  };
  ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(server_, &uri_share_access));
  
  const httpd_uri_t uri_download = {
    .uri       = "/*",
    .method    = HTTP_GET,
    .handler   = http_req_handler,
    .user_ctx  = this
  };
  ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(server_, &uri_download));

  ESP_LOGI(TAG, "Serveur HTTP démarré avec succès sur le port %d", local_port_);
  ESP_LOGI(TAG, "Interface utilisateur accessible à http://[ip-esp]:%d/", local_port_);
}

}  // namespace ftp_http_proxy
}  // namespace esphome
