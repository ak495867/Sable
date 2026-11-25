set -e

REPO="${SABLE_REPO:-ak495867/Sable}"
VERSION="${SABLE_VERSION:-latest}"

OS="$(uname -s)"
ARCH="$(uname -m)"

case "$OS" in
    Linux)
        OS_NAME="linux"
        ;;
    Darwin)
        OS_NAME="darwin"
        ;;
    *)
        echo "Unsupported operating system: $OS" >&2
        exit 1
        ;;
esac

case "$ARCH" in
    x86_64|amd64)
        ARCH_NAME="x86_64"
        ;;
    aarch64|arm64)
        ARCH_NAME="arm64"
        ;;
    *)
        echo "Unsupported architecture: $ARCH" >&2
        exit 1
        ;;
esac

if [ "$VERSION" = "latest" ]; then
    API_URL="https://api.github.com/repos/$REPO/releases/latest"
else
    API_URL="https://api.github.com/repos/$REPO/releases/tags/$VERSION"
fi

echo "Querying release information from $API_URL..."
AUTH_HEADER=()
if [ -n "$GITHUB_TOKEN" ]; then
    AUTH_HEADER=(-H "Authorization: Bearer $GITHUB_TOKEN")
fi

RELEASE_JSON="$(curl -fsSL "${AUTH_HEADER[@]}" -H "User-Agent: Sable-Installer" "$API_URL")"
ASSET_PATTERN="sable-${OS_NAME}-${ARCH_NAME}.tar.gz"
DOWNLOAD_URL="$(echo "$RELEASE_JSON" | grep -o 'https://[^" ]*'"$ASSET_PATTERN" | head -n 1)"

if [ -z "$DOWNLOAD_URL" ]; then
    echo "Could not find matching asset $ASSET_PATTERN in release" >&2
    exit 1
fi

INSTALL_DIR="${SABLE_INSTALL_DIR:-$HOME/.local/bin}"
mkdir -p "$INSTALL_DIR"

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

echo "Downloading $DOWNLOAD_URL..."
curl -fsSL -o "$TMP_DIR/sable.tar.gz" "$DOWNLOAD_URL"

echo "Extracting binary to $INSTALL_DIR..."
tar -xzf "$TMP_DIR/sable.tar.gz" -C "$TMP_DIR"
cp "$TMP_DIR/sable" "$INSTALL_DIR/sable"
chmod +x "$INSTALL_DIR/sable"

if ! echo "$PATH" | grep -q "$INSTALL_DIR"; then
    echo "Note: $INSTALL_DIR is not in your PATH."
    echo "Add it with: export PATH=\"$INSTALL_DIR:\$PATH\""
fi

echo "Successfully installed SABLE!"
"$INSTALL_DIR/sable" --version
