"""Prepend current build metadata while preserving the release description."""

import argparse
import re
import sys


START = "<!-- flon-release-build:start -->"
END = "<!-- flon-release-build:end -->"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("commit")
    parser.add_argument("updated_at")
    parser.add_argument("actions_url")
    args = parser.parse_args()

    body = sys.stdin.read()
    if body.startswith(START + "\n"):
        _, separator, body = body.partition(END)
        if not separator:
            parser.error("release build metadata has no closing marker")

    commit = args.commit
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        parser.error("expected a full Git commit ID")

    print(f"""{START}
## Latest build

- **Build commit** `{commit}`
- **Assets updated** {args.updated_at}
- **GitHub Actions** [View build]({args.actions_url})
{END}""")
    if body.strip():
        print()
        print(body.strip("\n"))


if __name__ == "__main__":
    main()
