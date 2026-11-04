# Contributing to NT-Box

Thank you for your interest in contributing to NT-Box! This document provides guidelines and instructions for contributing to the project.

---

## 🎯 Code of Conduct

Please be respectful and professional:
- Treat all contributors with respect
- Accept constructive criticism gracefully
- Focus on what's best for the community
- Report any harassment or inappropriate behavior

---

## 🚀 Getting Started

### 1. Fork the Repository

```bash
# Visit: https://github.com/imadxm44-stack/NTbox
# Click "Fork" button
# Clone your fork:
git clone https://github.com/YOUR-USERNAME/NTbox.git
cd NTbox
```

### 2. Set Upstream Remote

```bash
# Add original repo as upstream
git remote add upstream https://github.com/imadxm44-stack/NTbox.git

# Verify remotes
git remote -v
```

### 3. Create Feature Branch

```bash
# Update from upstream
git fetch upstream
git checkout upstream/main

# Create new branch
git checkout -b feature/your-feature-name

# Or for bug fixes
git checkout -b bugfix/issue-description
```

---

## 📝 Development Workflow

### Step 1: Make Your Changes

```bash
# Edit files as needed
# Write clean, well-documented code
# Follow project coding standards
```

### Step 2: Write Tests

```bash
# Add tests for new features
# Ensure all tests pass
make test

# Check code coverage
make coverage
```

### Step 3: Commit Your Changes

```bash
# Stage changes
git add .

# Write meaningful commit message
git commit -m "Add feature: description

- Detailed explanation
- What changed and why
- Reference issue number if applicable"

# Or use multiple commits for logical grouping
git commit -m "feat: add new component"
git commit -m "test: add tests for component"
```

### Step 4: Push to Your Fork

```bash
# Push branch
git push origin feature/your-feature-name

# Track upstream if needed
git push -u origin feature/your-feature-name
```

### Step 5: Submit Pull Request

```
# Go to GitHub
# Click "Compare & pull request"
# Fill in PR template:
  - Title: Brief description
  - Description: Detailed explanation
  - Changes: List of changes
  - Testing: How to test
  - Screenshots: If applicable
```

---

## 📋 PR Template

When submitting a pull request, please use this template:

```markdown
## Description
Brief description of changes

## Type of Change
- [ ] Bug fix
- [ ] New feature
- [ ] Performance improvement
- [ ] Documentation update
- [ ] Other (please specify)

## Changes Made
- Change 1
- Change 2
- Change 3

## Testing
How to test these changes:
- Run `make test`
- Run specific test: `make test-core`

## Screenshots (if applicable)
Add screenshots for UI changes

## Checklist
- [ ] My code follows style guidelines
- [ ] I have commented complex sections
- [ ] I have added tests
- [ ] All tests pass
- [ ] Documentation is updated
- [ ] No breaking changes
```

---

## 🎨 Coding Standards

### C Code Style

```c
// File headers
/*
 * filename.c - Brief description
 * Author: Your Name
 * Date: 2026-11-04
 */

#include "header.h"

// Function naming: snake_case
void process_data(int *data, size_t size) {
    // Variables: snake_case
    int buffer_size = 1024;
    
    // Constants: UPPER_SNAKE_CASE
    const int MAX_RETRIES = 3;
    
    // Indentation: 4 spaces
    for (int i = 0; i < size; i++) {
        data[i] = process_item(data[i]);
    }
    
    return;
}

// Comments explain WHY, not WHAT
// Good: Convert bytes to megabytes for display
int mb = bytes / (1024 * 1024);

// Bad: Divide bytes by 1048576
```

### Documentation Standards

```c
/**
 * process_data - Process input data array
 * @data: Pointer to data array
 * @size: Size of array
 *
 * Processes each element in the data array and returns
 * the count of successfully processed items.
 *
 * Return: Number of processed items, or -1 on error
 */
int process_data(int *data, size_t size);
```

### Commit Message Format

```
type: subject

body

footer

Types:
  feat: New feature
  fix: Bug fix
  docs: Documentation
  style: Formatting
  refactor: Code restructuring
  test: Adding tests
  chore: Maintenance

Subject: Imperative, present tense, max 50 chars
Body: Explain what and why, max 72 chars per line
Footer: Reference issues, breaking changes
```

---

## 🧪 Testing Requirements

### For All Contributions

```bash
# Run full test suite
make test

# All tests must pass
# Expected: 100% success rate
```

### For New Features

```bash
# Add tests in tools/ntboxd/test_*.c
# Test coverage minimum: 80%
# Run coverage report
make coverage
```

### For Bug Fixes

```bash
# Add test case that reproduces the bug
# Verify fix resolves the issue
# Ensure no regressions
make test-all
```

---

## 📚 Documentation

### Update These Files

- **README.md** - If changing project overview
- **docs/** - Add technical documentation
- **INSTALL.md** - If changing installation process
- **Code Comments** - For complex logic

### Documentation Quality

- Clear and concise
- Use markdown formatting
- Include examples where helpful
- Update table of contents
- Proofread for typos

---

## 🔍 Code Review Process

### What We Look For

✅ **Good Signs:**
- Clean, readable code
- Proper error handling
- Comprehensive tests
- Good documentation
- Follows project style
- Addresses the issue completely

❌ **Concerns:**
- No tests provided
- Poor code formatting
- Incomplete implementation
- Missing documentation
- Unrelated changes in same PR

### Review Feedback

- Be open to feedback
- Explain your reasoning
- Ask questions if unclear
- Update code based on review
- Respond to all comments

---

## 🐛 Reporting Bugs

### Before Reporting

- Search existing issues
- Check latest development branch
- Verify it's not a configuration issue

### Bug Report Format

```markdown
## Description
Brief description of the bug

## Steps to Reproduce
1. Step 1
2. Step 2
3. Step 3

## Expected Behavior
What should happen

## Actual Behavior
What actually happens

## Environment
- OS: Ubuntu 20.04
- Kernel: 5.4.0
- NT-Box Version: Latest
- Build: make/cmake

## Error Messages
```
Paste full error output here
```

## Additional Context
Any other relevant information
```

---

## 💡 Feature Requests

### Before Requesting

- Check if feature already exists
- Search for similar requests
- Consider implementation difficulty

### Feature Request Format

```markdown
## Description
What feature do you want?

## Use Case
Why do you need it?

## Current Behavior
How does it work now?

## Proposed Solution
How should it work?

## Alternatives Considered
Other approaches?

## Additional Context
Any other details?
```

---

## 📦 Development Setup

### Local Development

```bash
# Clone repository
git clone https://github.com/YOUR-USERNAME/NTbox.git
cd NTbox

# Install dev dependencies
sudo apt-get install -y \
    build-essential \
    gdb \
    valgrind \
    lcov

# Build for development
make clean
make DEBUG=1 -j$(nproc)

# Run tests
make test
```

### Using VSCode

```bash
# Install extensions:
# - C/C++ (Microsoft)
# - Makefile Tools
# - Git History

# Create .vscode/settings.json
{
    "C_Cpp.intelliSenseEngine": "Default",
    "files.exclude": {
        "**/*.o": true,
        "**/*.d": true
    }
}
```

---

## 🚢 Release Process

### Version Numbering

We follow Semantic Versioning: MAJOR.MINOR.PATCH

Examples:
- 1.0.0 - First stable release
- 1.1.0 - New features (minor)
- 1.1.1 - Bug fix (patch)

### Contributor Recognition

All contributors are recognized in:
- CONTRIBUTORS.md file
- Release notes
- GitHub contributors page

---

## 💬 Communication

### Where to Ask Questions

- **Bug Reports:** [GitHub Issues](https://github.com/imadxm44-stack/NTbox/issues)
- **General Questions:** [GitHub Discussions](https://github.com/imadxm44-stack/NTbox/discussions)
- **Quick Chat:** Email imadix44@example.com
- **Real-time:** Check community channels

### Response Time

- Bug reports: 24-48 hours
- Feature requests: 1-2 weeks
- PRs: 3-5 business days

---

## 🎓 Learning Resources

### Understanding the Project

1. Read README.md
2. Review QUICK_START.md
3. Study docs/ARCHITECTURE.md
4. Explore source code
5. Run examples

### Getting Help

- Browse existing issues
- Check documentation
- Ask in discussions
- Review pull requests
- Study test cases

---

## 📋 Contribution Ideas

### Good for Beginners
- Documentation improvements
- Bug fixes (marked as "good-first-issue")
- Test coverage
- Example programs
- Error messages improvement

### Intermediate
- New NT API stubs
- Performance optimization
- Code refactoring
- Tool improvements
- Feature implementation

### Advanced
- GPU support implementation
- GUI development
- Architecture changes
- Performance analysis
- Security improvements

---

## ⚖️ License

By contributing, you agree that:
- Your contributions are under MIT License
- You own the copyright to your work
- You have the right to contribute
- You understand the license terms

---

## 🙏 Thank You!

Your contributions help make NT-Box better for everyone.

We appreciate:
- Code contributions
- Documentation improvements
- Bug reports
- Feature suggestions
- Community support

---

## 📞 Need Help?

- **Documentation:** [docs/](docs/)
- **Issues:** [GitHub Issues](https://github.com/imadxm44-stack/NTbox/issues)
- **Discussions:** [GitHub Discussions](https://github.com/imadxm44-stack/NTbox/discussions)
- **Email:** imadix44@example.com

---

**Last Updated:** November 2026  
**Status:** Open to all contributions  
**License:** MIT License
