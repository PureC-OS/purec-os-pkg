import json, os, sys
api, repo, tag = sys.argv[1], sys.argv[2], sys.argv[3]
files = sys.argv[4:]
pre = False
if files and files[0] == "--pre":
    pre = True
    files = files[1:]
d = os.path.join(api, "repos", repo)
os.makedirs(d, exist_ok=True)
p = os.path.join(d, "releases?per_page=30")
rel = json.load(open(p)) if os.path.exists(p) else []
assets = [{"name": os.path.basename(f), "size": os.path.getsize(f), "browser_download_url": "file://" + os.path.abspath(f)} for f in files]
rel.insert(0, {"tag_name": tag, "name": "Release " + tag, "draft": False, "prerelease": pre, "assets": assets})
json.dump(rel, open(p, "w"), indent=1)
