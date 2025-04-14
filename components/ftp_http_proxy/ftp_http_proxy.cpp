#include "esp_wifi.h"
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
#include <cctype> // Pour std::tolower

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

  // Supprimer les liens de partage expirés
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
  
  // Générer un token aléatoire
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
  if (!server || !username || !password) {
    ESP_LOGE(TAG, "Paramètres FTP invalides");
    return false;
  }

  // Résolution DNS
  struct hostent *ftp_host = gethostbyname(server);
  if (!ftp_host) {
    ESP_LOGE(TAG, "Échec de la résolution DNS pour %s: %d", server, h_errno);
    return false;
  }

  // Création du socket
  sock = socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    ESP_LOGE(TAG, "Échec de création du socket : %d", errno);
    return false;
  }

  // Configuration des options du socket
  int flag = 1;
  if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag)) < 0) {
    ESP_LOGW(TAG, "Échec de configuration SO_KEEPALIVE: %d", errno);
  }
  
  int rcvbuf = 16384;
  if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
    ESP_LOGW(TAG, "Échec de configuration SO_RCVBUF: %d", errno);
  }

  struct timeval timeout = {.tv_sec = 10, .tv_usec = 0};
  if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
    ESP_LOGW(TAG, "Échec de configuration SO_RCVTIMEO: %d", errno);
  }
  
  if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
    ESP_LOGW(TAG, "Échec de configuration SO_SNDTIMEO: %d", errno);
  }

  // Connexion au serveur FTP
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(21);  // Port FTP standard
  
  // S'assurer que l'adresse est correctement assignée
  if (ftp_host->h_addrtype == AF_INET && ftp_host->h_addr_list[0] != NULL) {
    memcpy(&server_addr.sin_addr, ftp_host->h_addr_list[0], sizeof(struct in_addr));
  } else {
    ESP_LOGE(TAG, "Format d'adresse hôte non pris en charge");
    close(sock);
    sock = -1;
    return false;
  }

  if (connect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
    ESP_LOGE(TAG, "Échec de connexion FTP à %s : %d", server, errno);
    close(sock);
    sock = -1;
    return false;
  }

  // Réception du message de bienvenue
  char buffer[512];
  int bytes_received = recv(sock, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0) {
    ESP_LOGE(TAG, "Pas de réponse du serveur FTP: %d", errno);
    close(sock);
    sock = -1;
    return false;
  }
  
  buffer[bytes_received] = '\0';
  if (!strstr(buffer, "220 ")) {
    ESP_LOGE(TAG, "Message de bienvenue FTP non reconnu: %s", buffer);
    close(sock);
    sock = -1;
    return false;
  }

  // Envoi du nom d'utilisateur
  snprintf(buffer, sizeof(buffer), "USER %s\r\n", username);
  if (send(sock, buffer, strlen(buffer), 0) <= 0) {
    ESP_LOGE(TAG, "Échec d'envoi de la commande USER: %d", errno);
    close(sock);
    sock = -1;
    return false;
  }
  
  bytes_received = recv(sock, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0) {
    ESP_LOGE(TAG, "Pas de réponse à la commande USER: %d", errno);
    close(sock);
    sock = -1;
    return false;
  }
  buffer[bytes_received] = '\0';
  
  // Certains serveurs peuvent directement accepter l'utilisateur sans mot de passe
  if (strstr(buffer, "230 ")) {
    // Déjà authentifié
    ESP_LOGI(TAG, "Authentification FTP réussie sans mot de passe");
  } else if (!strstr(buffer, "331 ")) {
    ESP_LOGE(TAG, "Réponse USER inattendue: %s", buffer);
    close(sock);
    sock = -1;
    return false;
  } else {
    // Envoi du mot de passe
    snprintf(buffer, sizeof(buffer), "PASS %s\r\n", password);
    if (send(sock, buffer, strlen(buffer), 0) <= 0) {
      ESP_LOGE(TAG, "Échec d'envoi de la commande PASS: %d", errno);
      close(sock);
      sock = -1;
      return false;
    }
    
    bytes_received = recv(sock, buffer, sizeof(buffer) - 1, 0);
    if (bytes_received <= 0) {
      ESP_LOGE(TAG, "Pas de réponse à la commande PASS: %d", errno);
      close(sock);
      sock = -1;
      return false;
    }
    buffer[bytes_received] = '\0';
    
    if (!strstr(buffer, "230 ")) {
      ESP_LOGE(TAG, "Authentification FTP échouée: %s", buffer);
      close(sock);
      sock = -1;
      return false;
    }
  }

  // Passage en mode binaire
  if (send(sock, "TYPE I\r\n", 8, 0) <= 0) {
    ESP_LOGE(TAG, "Échec d'envoi de la commande TYPE I: %d", errno);
    close(sock);
    sock = -1;
    return false;
  }
  
  bytes_received = recv(sock, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "200 ")) {
    ESP_LOGE(TAG, "Échec du passage en mode binaire: %s", buffer);
    close(sock);
    sock = -1;
    return false;
  }

  ESP_LOGI(TAG, "Connexion FTP établie avec succès");
  return true;
}

void FTPHTTPProxy::file_transfer_task(void* param) {
  FileTransferContext* ctx = (FileTransferContext*)param;
  if (!ctx) {
    ESP_LOGE(TAG, "Contexte de transfert invalide");
    vTaskDelete(NULL);
    return;
  }
  
  // S'inscrire au watchdog
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_task_wdt_add(NULL));
  
  ESP_LOGI(TAG, "Démarrage du transfert pour %s", ctx->remote_path.c_str());
  
  FTPHTTPProxy proxy_instance;
  int ftp_sock = -1;
  int data_sock = -1;
  bool success = false;
  int bytes_received = 0;

  // Taille du buffer de transfert
  const int buffer_size = 16384;
  
  // Allocation plus sûre avec gestion d'erreur
  char* buffer = nullptr;
  
  // Tenter d'abord d'allouer en SPIRAM si disponible
  buffer = (char*)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  
  // Si l'allocation SPIRAM échoue, essayer la mémoire interne
  if (!buffer) {
    buffer = (char*)heap_caps_malloc(buffer_size, MALLOC_CAP_8BIT);
    
    // Si toutes les allocations échouent, abandonner proprement
    if (!buffer) {
      ESP_LOGE(TAG, "Échec d'allocation pour le buffer de transfert");
      httpd_resp_send_err(ctx->req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erreur mémoire");
      delete ctx;
      ESP_ERROR_CHECK_WITHOUT_ABORT(esp_task_wdt_delete(NULL));
      vTaskDelete(NULL);
      return;
    }
  }

  // Vérifier la validité du chemin de fichier
  if (ctx->remote_path.empty()) {
    ESP_LOGE(TAG, "Chemin de fichier distant vide");
    free(buffer);
    httpd_resp_send_err(ctx->req, HTTPD_404_NOT_FOUND, "Fichier non spécifié");
    delete ctx;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_task_wdt_delete(NULL));
    vTaskDelete(NULL);
    return;
  }

  // Vérifier que la connexion FTP est réussie
  if (!proxy_instance.connect_to_ftp(ftp_sock, ctx->ftp_server.c_str(), ctx->username.c_str(), ctx->password.c_str())) {
    ESP_LOGE(TAG, "Échec de connexion FTP");
    free(buffer);
    httpd_resp_send_err(ctx->req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erreur de connexion au serveur FTP");
    delete ctx;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_task_wdt_delete(NULL));
    vTaskDelete(NULL);
    return;
  }

  // Définition du type MIME en fonction de l'extension
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
  } else if (extension == ".flac") {
    httpd_resp_set_type(ctx->req, "audio/flac");
  } else if (extension == ".mp4") {
    httpd_resp_set_type(ctx->req, "video/mp4");
  } else if (extension == ".pdf") {
    httpd_resp_set_type(ctx->req, "application/pdf");
  } else if (extension == ".jpg" || extension == ".jpeg") {
    httpd_resp_set_type(ctx->req, "image/jpeg");
  } else if (extension == ".png") {
    httpd_resp_set_type(ctx->req, "image/png");
  } else {
    // Forcer le téléchargement pour les types inconnus
    httpd_resp_set_type(ctx->req, "application/octet-stream");
    std::string filename = ctx->remote_path;
    size_t slash_pos = ctx->remote_path.find_last_of('/');
    if (slash_pos != std::string::npos) {
      filename = ctx->remote_path.substr(slash_pos + 1);
    }
    std::string header = "attachment; filename=\"" + filename + "\"";
    httpd_resp_set_hdr(ctx->req, "Content-Disposition", header.c_str());
  }
  
  // Activer explicitement le mode chunked pour les gros fichiers
  httpd_resp_set_hdr(ctx->req, "Transfer-Encoding", "chunked");

  // Passer en mode passif et récupérer les paramètres de connexion de données
  if (send(ftp_sock, "PASV\r\n", 6, 0) <= 0) {
    ESP_LOGE(TAG, "Échec d'envoi de la commande PASV: %d", errno);
    free(buffer);
    close(ftp_sock);
    httpd_resp_send_err(ctx->req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erreur de transfert de fichier");
    delete ctx;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_task_wdt_delete(NULL));
    vTaskDelete(NULL);
    return;
  }
  
  bytes_received = recv(ftp_sock, buffer, buffer_size - 1, 0);
  if (bytes_received <= 0) {
    ESP_LOGE(TAG, "Erreur de réception en mode passif: %d", errno);
    free(buffer);
    close(ftp_sock);
    httpd_resp_send_err(ctx->req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erreur de transfert de fichier");
    delete ctx;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_task_wdt_delete(NULL));
    vTaskDelete(NULL);
    return;
  }
  
  buffer[bytes_received] = '\0';
  
  if (!strstr(buffer, "227 ")) {
    ESP_LOGE(TAG, "Réponse PASV incorrecte: %s", buffer);
    free(buffer);
    close(ftp_sock);
    httpd_resp_send_err(ctx->req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erreur de transfert de fichier");
    delete ctx;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_task_wdt_delete(NULL));
    vTaskDelete(NULL);
    return;
  }

  // Analyse de la réponse PASV pour extraire l'adresse IP et le port
  char *pasv_start = strchr(buffer, '(');
  if (!pasv_start) {
    ESP_LOGE(TAG, "Format PASV incorrect: parenthèse non trouvée");
    free(buffer);
    close(ftp_sock);
    httpd_resp_send_err(ctx->req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erreur de transfert de fichier");
    delete ctx;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_task_wdt_delete(NULL));
    vTaskDelete(NULL);
    return;
  }
  
  int ip[4], port[2];
  if (sscanf(pasv_start, "(%d,%d,%d,%d,%d,%d)", &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]) != 6) {
    ESP_LOGE(TAG, "Format PASV incorrect: impossible de parser les valeurs");
    free(buffer);
    close(ftp_sock);
    httpd_resp_send_err(ctx->req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erreur de transfert de fichier");
    delete ctx;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_task_wdt_delete(NULL));
    vTaskDelete(NULL);
    return;
  }
  
  int data_port = port[0] * 256 + port[1];
  
  // Création du socket de données
  data_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (data_sock < 0) {
    ESP_LOGE(TAG, "Échec de création du socket de données: %d", errno);
    free(buffer);
    close(ftp_sock);
    httpd_resp_send_err(ctx->req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erreur de transfert de fichier");
    delete ctx;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_task_wdt_delete(NULL));
    vTaskDelete(NULL);
    return;
  }
  
  // Configuration des options du socket de données
  flag = 1;
  if (setsockopt(data_sock, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag)) < 0) {
    ESP_LOGW(TAG, "Échec de configuration SO_KEEPALIVE pour data_sock: %d", errno);
  }
  
  int rcvbuf = 32768;
  if (setsockopt(data_sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
    ESP_LOGW(TAG, "Échec de configuration SO_RCVBUF pour data_sock: %d", errno);
  }
  
  struct timeval data_timeout = {.tv_sec = 15, .tv_usec = 0};  // Délai augmenté
  if (setsockopt(data_sock, SOL_SOCKET, SO_RCVTIMEO, &data_timeout, sizeof(data_timeout)) < 0) {
    ESP_LOGW(TAG, "Échec de configuration SO_RCVTIMEO pour data_sock: %d", errno);
  }
  
  // Connexion au port de données
  struct sockaddr_in data_addr;
  memset(&data_addr, 0, sizeof(data_addr));
  data_addr.sin_family = AF_INET;
  data_addr.sin_port = htons(data_port);
  
  // Utilisation de inet_addr pour définir l'adresse IP
  char ip_str[16];
  snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  data_addr.sin_addr.s_addr = inet_addr(ip_str);
  
  if (connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) != 0) {
    ESP_LOGE(TAG, "Échec de connexion au port de données: %d", errno);
    free(buffer);
    close(data_sock);
    close(ftp_sock);
    httpd_resp_send_err(ctx->req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erreur de transfert de fichier");
    delete ctx;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_task_wdt_delete(NULL));
    vTaskDelete(NULL);
    return;
  }

  // Envoi de la commande RETR pour récupérer le fichier
  snprintf(buffer, buffer_size, "RETR %s\r\n", ctx->remote_path.c_str());
  int sent = send(ftp_sock, buffer, strlen(buffer), 0);
  if (sent <= 0) {
    ESP_LOGE(TAG, "Échec d'envoi de la commande RETR: %d", errno);
    free(buffer);
    close(data_sock);
    close(ftp_sock);
    httpd_resp_send_err(ctx->req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erreur de transfert de fichier");
    delete ctx;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_task_wdt_delete(NULL));
    vTaskDelete(NULL);
    return;
  }

  // Vérification de la réponse à la commande RETR
  bytes_received = recv(ftp_sock, buffer, buffer_size - 1, 0);
  if (bytes_received <= 0) {
    ESP_LOGE(TAG, "Échec de réception de la réponse RETR: %d", errno);
    free(buffer);
    close(data_sock);
    close(ftp_sock);
    httpd_resp_send_err(ctx->req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erreur de transfert de fichier");
    delete ctx;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_task_wdt_delete(NULL));
    vTaskDelete(NULL);
    return;
  }
  
  buffer[bytes_received] = '\0';
  
  if (!strstr(buffer, "150 ") && !strstr(buffer, "125 ")) {
    ESP_LOGE(TAG, "Fichier non trouvé ou inaccessible: %s", buffer);
    free(buffer);
    close(data_sock);
    close(ftp_sock);
    httpd_resp_send_err(ctx->req, HTTPD_404_NOT_FOUND, "Fichier non trouvé ou inaccessible");
    delete ctx;
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_task_wdt_delete(NULL));
    vTaskDelete(NULL);
    return;
  }
  
  ESP_LOGI(TAG, "Téléchargement du fichier %s démarré", ctx->remote_path.c_str());

  // Transfert des données
  size_t total_bytes_transferred = 0;
  esp_err_t err = ESP_OK;
  
  // Définir la taille des chunks plus petite pour les gros fichiers
  const int chunk_size = 4096;  // Plus petit que buffer_size
  
  // Boucle de transfert de données
  while (true) {
    // Réinitialiser le watchdog régulièrement
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_task_wdt_reset());
    
    bytes_received = recv(data_sock, buffer, buffer_size, 0);
    if (bytes_received <= 0) {
      if (bytes_received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        ESP_LOGE(TAG, "Erreur de réception des données: %d", errno);
      } else {
        // Fin normale du fichier
        ESP_LOGI(TAG, "Fin du transfert de données");
      }
      break;
    }
    
    total_bytes_transferred += bytes_received;
    
    // Vérification de la mémoire disponible
    if (esp_get_free_heap_size() < 15000) {
      ESP_LOGW(TAG, "Mémoire critique: %d octets", esp_get_free_heap_size());
      vTaskDelay(pdMS_TO_TICKS(50));  // Pause pour permettre la libération de mémoire
    }
    
    // Envoyer en petits chunks au lieu d'un gros chunk
    if (bytes_received > chunk_size) {
      for (int i = 0; i < bytes_received; i += chunk_size) {
        int current_chunk = std::min(chunk_size, bytes_received - i);
        err = httpd_resp_send_chunk(ctx->req, buffer + i, current_chunk);
        if (err != ESP_OK) {
          ESP_LOGE(TAG, "Échec d'envoi du chunk: %s", esp_err_to_name(err));
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));  // Petit délai entre les chunks
      }
    } else {
      err = httpd_resp_send_chunk(ctx->req, buffer, bytes_received);
    }
    
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Échec d'envoi au client HTTP: %s", esp_err_to_name(err));
      break;
    }
    
    // Afficher le progrès périodiquement
    if (total_bytes_transferred % (256 * 1024) == 0) {
      ESP_LOGI(TAG, "Transfert en cours: %.2f MB", total_bytes_transferred / (1024.0 * 1024.0));
      ESP_ERROR_CHECK_WITHOUT_ABORT(esp_task_wdt_reset());  // Réinitialiser le watchdog
      vTaskDelay(pdMS_TO_TICKS(5));  // Petit délai pour éviter la saturation
    }
  }
  
  // Fermeture du socket de données
  if (data_sock != -1) {
    close(data_sock);
    data_sock = -1;
  }
  
  // Vérification de la fin du transfert FTP
  bytes_received = recv(ftp_sock, buffer, buffer_size - 1, 0);
  if (bytes_received > 0) {
    buffer[bytes_received] = '\0';
    if (strstr(buffer, "226 ") || strstr(buffer, "250 ")) {
      ESP_LOGI(TAG, "Transfert terminé avec succès: %.2f KB (%.2f MB)", 
               total_bytes_transferred / 1024.0,
               total_bytes_transferred / (1024.0 * 1024.0));
      success = true;
    } else {
      ESP_LOGW(TAG, "Fin de transfert avec message inattendu: %s", buffer);
    }
  } else {
    ESP_LOGW(TAG, "Pas de réponse de fin de transfert du serveur FTP");
  }

  // Nettoyage des ressources
  if (buffer) {
    free(buffer);
    buffer = nullptr;
  }
  
  if (ftp_sock != -1) {
    send(ftp_sock, "QUIT\r\n", 6, 0);
    close(ftp_sock);
    ftp_sock = -1;
  }
  
  // Finalisation de la réponse HTTP
  if (!success) {
    httpd_resp_send_err(ctx->req, HTTPD_500_INTERNAL_SERVER_ERROR, "Erreur de transfert de fichier");
  } else {
    // Terminer le mode chunked
    httpd_resp_send_chunk(ctx->req, NULL, 0);
  }
  
  // Libération sécurisée du contexte
  delete ctx;
  
  // Se désinscrire du watchdog avant de terminer
  ESP_ERROR_CHECK_WITHOUT_ABORT(esp_task_wdt_delete(NULL));
  
  vTaskDelete(NULL);
}

bool FTPHTTPProxy::list_ftp_directory(const std::string &dir_path, httpd_req_t *req) {
  int ftp_sock = -1;
  int data_sock = -1;
  char buffer[1024];
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
  
  // Corriger la connexion au port de données en utilisant inet_addr
  char ip_str[16];
  snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  data_addr.sin_addr.s_addr = inet_addr(ip_str);
  
  if (connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) != 0) {
    close(ftp_sock);
    close(data_sock);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "[]", 2);
    return false;
  }
  
  if (dir_path.empty()) {
    send(ftp_sock, "LIST\r\n", 6, 0);
  } else {
    snprintf(buffer, sizeof(buffer), "LIST %s\r\n", dir_path.c_str());
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
      // Corriger la détection des répertoires
        if (perms[0] == 'd') {
          // C'est un répertoire
          file_list += "{\"name\":\"" + std::string(filename) + "\",";
          file_list += "\"path\":\"" + (dir_path.empty() ? "" : dir_path + "/") + std::string(filename) + "\",";
          file_list += "\"type\":\"directory\",";
          file_list += "\"size\":0,";
          file_list += "\"shareable\":false}";
        } else {
          // C'est un fichier
          file_list += "{\"name\":\"" + std::string(filename) + "\",";
          file_list += "\"path\":\"" + (dir_path.empty() ? "" : dir_path + "/") + std::string(filename) + "\",";
          file_list += "\"type\":\"file\",";
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
