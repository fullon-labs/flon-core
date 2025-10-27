#!/bin/bash

# Installation script for transaction_history_plugin dependencies

set -e

echo "Installing dependencies for transaction_history_plugin..."

# Function to detect OS
detect_os() {
    if [[ "$OSTYPE" == "linux-gnu"* ]]; then
        if [ -f /etc/debian_version ]; then
            echo "debian"
        elif [ -f /etc/redhat-release ]; then
            echo "redhat"
        else
            echo "linux"
        fi
    elif [[ "$OSTYPE" == "darwin"* ]]; then
        echo "macos"
    else
        echo "unknown"
    fi
}

# Install RocksDB based on OS
install_rocksdb() {
    local os=$(detect_os)

    case $os in
        debian)
            echo "Installing RocksDB on Debian/Ubuntu..."
            sudo apt-get update
            sudo apt-get install -y librocksdb-dev pkg-config
            ;;
        redhat)
            echo "Installing RocksDB on Red Hat/CentOS/Fedora..."
            if command -v dnf &> /dev/null; then
                sudo dnf install -y rocksdb-devel pkgconfig
            else
                sudo yum install -y rocksdb-devel pkgconfig
            fi
            ;;
        macos)
            echo "Installing RocksDB on macOS..."
            if command -v brew &> /dev/null; then
                brew install rocksdb pkg-config
            else
                echo "Please install Homebrew first: https://brew.sh/"
                exit 1
            fi
            ;;
        *)
            echo "Unknown OS. Please install RocksDB manually."
            echo "See: https://github.com/facebook/rocksdb/blob/main/INSTALL.md"
            exit 1
            ;;
    esac
}

# Check if RocksDB is already installed
check_rocksdb() {
    if pkg-config --exists rocksdb; then
        echo "RocksDB is already installed:"
        pkg-config --modversion rocksdb
        return 0
    elif [ -f /usr/include/rocksdb/db.h ]; then
        echo "RocksDB headers found in /usr/include/rocksdb/"
        return 0
    else
        return 1
    fi
}

# Main installation
main() {
    echo "Checking for RocksDB installation..."

    if check_rocksdb; then
        echo "✅ RocksDB is already installed and ready to use."
    else
        echo "❌ RocksDB not found. Installing..."
        install_rocksdb

        # Verify installation
        if check_rocksdb; then
            echo "✅ RocksDB successfully installed!"
        else
            echo "❌ RocksDB installation failed. Please install manually."
            exit 1
        fi
    fi

    echo ""
    echo "Dependencies installation complete!"
    echo "You can now build the transaction_history_plugin:"
    echo "  mkdir build && cd build"
    echo "  cmake .."
    echo "  make transaction_history_plugin"
}

main "$@"
