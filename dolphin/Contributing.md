# Contributing to the Brawl Online Dolphin

This directory is the Dolphin of Brawl Online. Make changes to it in this repository.

## Pull requests

- Open pull requests against the `main` branch of this repository.
- Make one change in each pull request.
- If your change touches rollback, snapshots or netplay, run the tests in `harness/`.

## Legal

- Do not contribute if you know confidential information about the GameCube or Wii.
  This includes information from a non-disclosure agreement or from leaked material.
- Do not share or ask for pirated games.
- Your contributions are licensed under the GNU GPL, version 2 or later.

## Code style

- Use the style of the code around your change.
- Format C++ code with clang-format 19. The settings are in `Source/.clang-format`.
- To format the files that you staged, run this command:
  ```
  git diff --cached --name-only | grep -E '[.](cpp|h|mm)$' | xargs -I {} clang-format -i {}
  ```
- Commit files with LF line endings.
