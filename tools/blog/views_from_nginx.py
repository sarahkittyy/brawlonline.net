"""Blog views from before accounts counted them: the readers in prod's nginx log, by the same
rule as accounts (server/crates/accounts/src/blog.rs, count_view / is_reader): one view per reader
(address, /64 for IPv6, + User-Agent) per post per UTC day, browsers only, GET 200, not the
preview link. Prints an UPDATE per post to add them to blog_posts.views.

    ssh debian@sarahvps2 'sudo grep -F "GET /blog/" /var/log/nginx/brawl-online.access.log' \
      | python -I tools/blog/views_from_nginx.py --before 2026-11-14T02:00:00Z

--before: when the release with the counter went live (accounts counts from then on). Readers
who came both before and after it on that UTC day count twice; that's all.
"""

import argparse
import collections
import datetime
import ipaddress
import re
import sys

# The same list as blog::is_reader.
NOT_PEOPLE = [
    "bot/", "bot;", "bot)", "+http", "crawl", "spider", "slurp", "preview", "facebookexternalhit",
    "facebot", "meta-externalagent", "meta-webindexer", "trendiction", "headless", "lighthouse",
    "chatgpt", "claude", "perplexity", "bytespider", "whatsapp", "telegram", "slack", "discord",
    "embedly", "iframely", "google-", "python", "curl", "wget", "go-http", "okhttp", "node-fetch",
    "java/",
]

LINE = re.compile(r'^(\S+) \S+ \S+ \[([^\]]+)\] "GET (/blog/([a-z0-9-]{1,80}))(\?[^ "]*)? [^"]*" 200 \d+ "[^"]*" "([^"]*)"')
NOT_POSTS = {"write", "media", "sitemap"}


def is_reader(ua: str) -> bool:
    return ua.startswith("Mozilla/") and not any(w in ua.lower() for w in NOT_PEOPLE)


def ip_key(ip: str) -> str:
    a = ipaddress.ip_address(ip)
    if a.version == 6:
        return str(ipaddress.ip_network(f"{ip}/64", strict=False).network_address)
    return ip


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--before", required=True, help="UTC time the counter went live, e.g. 2026-11-14T02:00:00Z")
    args = ap.parse_args()
    before = datetime.datetime.fromisoformat(args.before.replace("Z", "+00:00"))
    seen = collections.defaultdict(set)
    for line in sys.stdin.buffer:
        m = LINE.match(line.decode("utf-8", "replace"))
        if not m:
            continue
        ip, ts, _path, slug, query, ua = m.groups()
        if slug in NOT_POSTS or (query and "preview=" in query) or not is_reader(ua):
            continue
        t = datetime.datetime.strptime(ts, "%d/%b/%Y:%H:%M:%S %z")
        if t >= before:
            continue
        seen[slug].add((t.astimezone(datetime.timezone.utc).date(), ip_key(ip), ua))
    for slug, readers in sorted(seen.items()):
        print(f"UPDATE blog_posts SET views = views + {len(readers)} WHERE slug = '{slug}';  -- {len(readers)} views")


if __name__ == "__main__":
    main()
