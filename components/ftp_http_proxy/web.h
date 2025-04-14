#ifndef WEB_H
#define WEB_H

static const char* HTML_INDEX = R"=====(
<!DOCTYPE html>
<html lang="fr">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32 File Browser</title>
    <style>
        body { font-family: Arial, sans-serif; max-width: 800px; margin: 0 auto; padding: 20px; }
        h1 { color: #333; }
        .file-list { list-style: none; padding: 0; }
        .file-item { padding: 10px; border-bottom: 1px solid #eee; display: flex; justify-content: space-between; align-items: center; }
        .file-name { flex-grow: 1; }
        .file-actions { display: flex; gap: 10px; }
        .btn { padding: 6px 12px; border-radius: 4px; cursor: pointer; text-decoration: none; font-size: 14px; }
        .download-btn { background: #4CAF50; color: white; border: none; }
        .share-btn { background: #2196F3; color: white; border: none; }
        .toggle-btn { background: #FF9800; color: white; border: none; }
        .modal { display: none; position: fixed; top: 0; left: 0; width: 100%; height: 100%; background: rgba(0,0,0,0.5); align-items: center; justify-content: center; }
        .modal-content { background: white; padding: 20px; border-radius: 5px; width: 90%; max-width: 500px; }
        .close-btn { float: right; cursor: pointer; font-size: 20px; }
        .share-link { padding: 10px; background: #f5f5f5; border-radius: 4px; word-break: break-all; margin: 10px 0; }
        .copy-btn { background: #673AB7; color: white; border: none; padding: 5px 10px; cursor: pointer; border-radius: 4px; }
        .success-msg { color: green; display: none; }
        .shareable-badge { display: inline-block; background: #4CAF50; color: white; font-size: 10px; padding: 3px 6px; border-radius: 3px; margin-left: 5px; }
    </style>
</head>
<body>
    <h1>ESP32 File Browser</h1>
    <ul class="file-list">
        <!-- Files will be loaded here -->
    </ul>
    <div id="shareModal" class="modal">
        <div class="modal-content">
            <span class="close-btn">&times;</span>
            <h2>Partage de fichier</h2>
            <p>Lien de partage (valide pour <span id="expiryHours">24</span> heures):</p>
            <div class="share-link" id="shareLink"></div>
            <button class="copy-btn" onclick="copyShareLink()">Copier</button>
            <span class="success-msg" id="copySuccess">Lien copié!</span>
        </div>
    </div>
    <script>
        // Charger la liste des fichiers
        function loadFiles() {
            fetch('/api/files')
                .then(response => response.json())
                .then(files => {
                    const fileList = document.querySelector('.file-list');
                    fileList.innerHTML = '';
                    files.forEach(file => {
                        const li = document.createElement('li');
                        li.className = 'file-item';
                        const nameDiv = document.createElement('div');
                        nameDiv.className = 'file-name';
                        nameDiv.textContent = file.name;
                        if (file.shareable) {
                            const badge = document.createElement('span');
                            badge.className = 'shareable-badge';
                            badge.textContent = 'Partageable';
                            nameDiv.appendChild(badge);
                        }
                        const actionsDiv = document.createElement('div');
                        actionsDiv.className = 'file-actions';
                        const downloadBtn = document.createElement('a');
                        downloadBtn.className = 'btn download-btn';
                        downloadBtn.textContent = 'Télécharger';
                        downloadBtn.href = '/' + file.path;
                        actionsDiv.appendChild(downloadBtn);
                        // Bouton de partage uniquement pour les fichiers
                        if (file.type === 'file') {
                            const toggleBtn = document.createElement('button');
                            toggleBtn.className = 'btn toggle-btn';
                            toggleBtn.textContent = file.shareable ? 'Ne pas partager' : 'Rendre partageable';
                            toggleBtn.onclick = () => toggleShareable(file.path, !file.shareable);
                            actionsDiv.appendChild(toggleBtn);
                            if (file.shareable) {
                                const shareBtn = document.createElement('button');
                                shareBtn.className = 'btn share-btn';
                                shareBtn.textContent = 'Partager';
                                shareBtn.onclick = () => createShareLink(file.path);
                                actionsDiv.appendChild(shareBtn);
                            }
                        }
                        li.appendChild(nameDiv);
                        li.appendChild(actionsDiv);
                        fileList.appendChild(li);
                    });
                })
                .catch(error => console.error('Erreur lors du chargement des fichiers:', error));
        }
        // Activer/Désactiver le partage d'un fichier
        function toggleShareable(path, shareable) {
            fetch('/api/toggle-shareable', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ path: path, shareable: shareable })
            })
            .then(response => {
                if (response.ok) {
                    loadFiles(); // Recharger la liste des fichiers
                } else {
                    console.error('Erreur lors du changement de statut de partage');
                }
            })
            .catch(error => console.error('Erreur:', error));
        }
        // Créer un lien de partage
        function createShareLink(path) {
            fetch('/api/share', {
                method: 'POST',
                headers: { 'Content-Type': 'application/json' },
                body: JSON.stringify({ path: path, expiry: 24 })
            })
            .then(response => response.json())
            .then(data => {
                const shareLink = document.getElementById('shareLink');
                shareLink.textContent = window.location.origin + data.link;
                document.getElementById('expiryHours').textContent = data.expiry;
                const modal = document.getElementById('shareModal');
                modal.style.display = 'flex';
            })
            .catch(error => console.error('Erreur lors de la création du lien:', error));
        }
        // Copier le lien de partage
        function copyShareLink() {
            const shareLink = document.getElementById('shareLink').textContent;
            navigator.clipboard.writeText(shareLink).then(() => {
                const copySuccess = document.getElementById('copySuccess');
                copySuccess.style.display = 'inline';
                setTimeout(() => { copySuccess.style.display = 'none'; }, 2000);
            });
        }
        // Fermer la modal
        document.querySelector('.close-btn').onclick = function() {
            document.getElementById('shareModal').style.display = 'none';
        }
        // Fermer la modal si on clique en dehors
        window.onclick = function(event) {
            const modal = document.getElementById('shareModal');
            if (event.target == modal) {
                modal.style.display = 'none';
            }
        }
        // Charger les fichiers au démarrage
        document.addEventListener('DOMContentLoaded', loadFiles);
    </script>
</body>
</html>
)=====";

#endif // WEB_H
