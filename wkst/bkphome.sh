sudo tar --create --file=/mnt/Local/home-$(date +%F).tar.zst \
  --zstd \
  --preserve-permissions \
  --acls --xattrs --xattrs-include='*' \
  --numeric-owner \
  --sparse \
  --one-file-system \
  --exclude="$HOME/.cache" \
  --exclude="$HOME/.local/share/Trash" \
  -C /home "$USER"