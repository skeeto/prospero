import json, os, sys

FILE = "todo.json"

def load():
    if not os.path.exists(FILE):
        return []
    with open(FILE) as f:
        return json.load(f)

def save(items):
    with open(FILE, "w") as f:
        json.dump(items, f)

def main(args):
    items = load()
    if not args or args[0] == "list":
        for i, item in enumerate(items, 1):
            print(f"{i}. [ ] {item['text']}")
    elif args[0] == "add":
        items.append({"text": " ".join(args[1:])})
        save(items)
    else:
        print("usage: todo.py [list | add TEXT]", file=sys.stderr)
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
