# NT-Box Installation Guide

A comprehensive guide to install NT-Box on various Linux systems.

---

## 📋 Basic Requirements

### Required Software:
- **GCC** or **Clang** (version 9+)
- **Make** and **CMake** (version 3.10+)
- **Linux Headers** for current system kernel
- **Git** (to clone the project)
- **Python 3.6+** (optional, for tools)

### System Requirements:
- **RAM:** At least 2 GB
- **Storage:** 500 MB (for full build)
- **Permissions:** Root access (for installation in /usr/local)

---

## 🐧 Installation by Distribution

### Ubuntu / Debian

```bash
# Update package lists
sudo apt-get update

# Install basic requirements
sudo apt-get install -y \
    build-essential \
    linux-headers-$(uname -r) \
    cmake \
    git \
    make

# Install additional requirements (optional)
sudo apt-get install -y \
    clang \
    llvm \
    valgrind \
    gdb \
    python3-dev
```

### Fedora / RHEL / CentOS

```bash
# Install basic requirements
sudo dnf install -y \
    gcc \
    gcc-c++ \
    make \
    cmake \
    git \
    kernel-devel

# Install additional requirements (optional)
sudo dnf install -y \
    clang \
    llvm-devel \
    valgrind \
    gdb \
    python3-devel
```

### Arch Linux

```bash
# Install basic requirements
sudo pacman -S \
    base-devel \
    linux-headers \
    cmake \
    git \
    make

# Install additional requirements (optional)
sudo pacman -S \
    clang \
    llvm \
    valgrind \
    gdb
```

### openSUSE

```bash
# Install basic requirements
sudo zypper install -y \
    gcc \
    make \
    cmake \
    git \
    kernel-devel

# Install additional requirements (optional)
sudo zypper install -y \
    clang \
    llvm-devel \
    valgrind \
    gdb
```

---

## 🔧 Building from Source

### Step 1: Clone the Repository

```bash
# Clone the project
git clone https://github.com/imadxm44-stack/NTbox.git

# Navigate to directory
cd NTbox

# Check branch
git branch -a
```

### Step 2: Configure Build

```bash
# View build options
make help

# Basic configuration
./configure

# Advanced configuration (optional)
./configure --enable-debug --enable-optimization
```

### Step 3: Build the Project

```bash
# Clean previous builds
make clean

# Build with multiple cores
make -j$(nproc)

# Or specify number of cores
make -j4

# Build specific component
make core
make bridge
make hal
```

### Step 4: Run Tests

```bash
# Run all tests
make test

# Run specific test suite
make test-core
make test-bridge
make test-hal
make test-irp

# Run tests with verbose output
make test VERBOSE=1
```

### Step 5: Install System-wide (Optional)

```bash
# Install to /usr/local
sudo make install

# Or install to custom location
make install PREFIX=/opt/ntbox

# Verify installation
which ntbox
ntbox --version
```

---

## 📦 Development Installation

### For Contributors

```bash
# Clone with all branches
git clone --mirror https://github.com/imadxm44-stack/NTbox.git ntbox.git
cd ntbox.git

# Install development dependencies
sudo apt-get install -y \
    build-essential \
    gdb \
    valgrind \
    lcov \
    gcov

# Build with debugging
make DEBUG=1 -j$(nproc)

# Generate coverage report
make coverage
```

### Using CMake (Alternative)

```bash
# Create build directory
mkdir build
cd build

# Configure with CMake
cmake ..

# Or with options
cmake -DCMAKE_BUILD_TYPE=Debug -DENABLE_TESTS=ON ..

# Build
cmake --build . -j$(nproc)

# Run tests
ctest --output-on-failure

# Install
sudo cmake --install .
```

---

## 🧪 Verification

### Post-Installation Checks

```bash
# Check kernel module
lsmod | grep ntbox

# Verify main tool
ntbox --version
ntbox --help

# Check daemon
sudo systemctl status ntboxd

# Run verification script
./tools/verify.sh
```

### Test Suite Verification

```bash
# Run complete test suite
make verify

# Expected output: PASS with 100% success rate
# All tests should pass without errors
```

---

## 🚀 Quick Start After Installation

```bash
# Start the daemon
sudo systemctl start ntboxd

# Enable at boot
sudo systemctl enable ntboxd

# Run interactive shell
ntbox shell

# Run a specific application
ntbox run /path/to/application.exe

# Check status
ntbox info
```

---

## 🔍 Troubleshooting

### Build Errors

**Error: "gcc not found"**
```bash
# Install compiler
sudo apt-get install build-essential
```

**Error: "linux-headers not found"**
```bash
# Install headers for current kernel
sudo apt-get install linux-headers-$(uname -r)
```

**Error: "cmake version too old"**
```bash
# Upgrade cmake
sudo apt-get remove cmake
sudo apt-get install cmake
```

### Runtime Issues

**Module loading fails:**
```bash
# Check kernel version
uname -r

# Verify module
sudo modprobe ntbox

# Check logs
dmesg | tail -20
```

**Permission denied errors:**
```bash
# Check permissions
ls -la /dev/ntbox*

# Fix permissions
sudo chmod 666 /dev/ntbox
```

---

## 📋 Installation Options

### Minimal Installation
```bash
make minimal-install
# Installs only core components (~50 MB)
```

### Full Installation
```bash
sudo make install
# Installs all components including tools and documentation
```

### Development Installation
```bash
make dev-install
# Installs with debug symbols and development files
```

---

## 🔐 Security Considerations

- Always verify source before building
- Use checksums when downloading
- Keep system updated
- Review SECURITY.md for security guidelines

---

## 📞 Getting Help

If you encounter issues:

1. Check the [Troubleshooting Guide](docs/TROUBLESHOOTING.md)
2. Review existing [GitHub Issues](https://github.com/imadxm44-stack/NTbox/issues)
3. Ask in [GitHub Discussions](https://github.com/imadxm44-stack/NTbox/discussions)
4. Contact: imadix44@example.com

---

## ✅ Installation Complete!

After successful installation, you can:
- Read [Quick Start Guide](QUICK_START.md)
- Review [Architecture](docs/ARCHITECTURE.md)
- Start developing with [API Reference](docs/API_REFERENCE.md)
- Contribute via [Contributing Guide](CONTRIBUTING.md)

---

**Last Updated:** November 2026  
**Tested On:** Ubuntu 20.04+, Fedora 34+, Arch Linux, openSUSE  
**Status:** Production Ready ✅
