# Contributing to NanoBucket

As a solo developer, I maintain strict development practices to ensure code quality and maintainability. This guide outlines the processes I follow for development.

## Branching Strategy

I follow a strict Gitflow workflow to maintain a clean and organized codebase:

- **`main`**: Core branch with the latest shippable code. This branch is protected and maintains linear history.
- **`develop`**: Integration branch for ongoing development.
- **`feature/*`**: Branches for developing new capabilities. Create from `develop` and merge back to `develop`.
- **`fix/*`**: Branches for bug fixes. Create from `develop` and merge back to `develop`.
- **`release/*`**: Branches for preparing a new tag. Create from `develop` and merge into both `develop` and `main`.

## Commit Guidelines

- **Signed Commits**: All commits must be signed using `git commit -s` or `git commit --signoff`.
- **Linear History**: I maintain a linear history. Only squash operations are allowed; merging and rebase is not permitted as merging creates non-linear history and rebase removes signatures.
- **Commit Messages**: Write clear, concise commit messages that describe what was changed and why.

## Development Process

1. Create a feature or fix branch from `develop`.
2. Make changes and test thoroughly.
3. Rebase onto the latest `develop` to resolve conflicts.
4. Squash commits into meaningful units.
5. Merge back to `develop` via a pull request.

## Issue Tracking

I use GitHub issues to track tasks. Issue templates are available:

- [Bug Report](.github/ISSUE_TEMPLATE/bug_report.md)
- [Feature Request](.github/ISSUE_TEMPLATE/feature_request.md)

## Development Setup

See the [README.md](README.md) for setup instructions.
