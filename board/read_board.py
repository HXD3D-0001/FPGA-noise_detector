"""Read status and spectrum from an already running monitor via ETH0."""
import argparse
import ipaddress
import json
from urllib.request import ProxyHandler, build_opener

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("board_ip", type=ipaddress.IPv4Address, help="IPv4 address of the board")
    args = parser.parse_args()
    http = build_opener(ProxyHandler({}))
    for route in ("status", "spectrum"):
        with http.open(f"http://{args.board_ip}/api/{route}", timeout=10) as response:
            value = json.loads(response.read())
        print(route, json.dumps(value, ensure_ascii=False))

if __name__ == "__main__":
    main()
