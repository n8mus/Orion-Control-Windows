# History guard: rejects a push that brings back retired history.
# retired-commits.txt = commit IDs replaced in the 2026-11-12 rewrite.
# fingerprints.txt = SHA-256 of retired words (no text kept here).
# On a hit the ref is put back where it was (or deleted if new).
import hashlib, json, os, re, subprocess, sys
D = os.path.dirname(os.path.abspath(__file__))
def g(*a): return subprocess.run(["git", *a], capture_output=True, text=True).stdout
retired = set(open(f"{D}/retired-commits.txt").read().split())
prints = set(open(f"{D}/fingerprints.txt").read().split())
ev = json.load(open(os.environ["GITHUB_EVENT_PATH"]))
ref, before, after = ev["ref"], ev["before"], ev["after"]
if set(after) == {"0"}: sys.exit(0)
name = ref.split("/", 2)[2]
if set(before) == {"0"}:
    others = [r for r in g("for-each-ref", "--format=%(refname)", "refs/remotes/origin", "refs/tags").split()
              if r not in (f"refs/remotes/origin/{name}", f"refs/tags/{name}", "refs/remotes/origin/HEAD")]
    new = g("rev-list", after, "--not", *others).split()
else:
    new = g("rev-list", f"{before}..{after}").split()
def h(s): return hashlib.sha256(s.encode()).hexdigest()
def dirty(text):
    w = [x.strip(".,;:()[]`*") for x in re.sub(r'["\']', "", text.lower()).split()]
    return any(h(x) in prints for x in w) or any(h(a + " " + b) in prints for a, b in zip(w, w[1:]))
bad = [c for c in new if c in retired]
for c in new:
    if bad: break
    msg = g("log", "-1", "--format=%B", c)
    added = "\n".join(l[1:] for l in g("show", "--format=", "--unified=0", c).splitlines()
                      if l.startswith("+") and not l.startswith("+++"))
    if dirty(msg) or dirty(added): bad.append(c)
if not bad:
    print(f"clean: {len(new)} new commit(s)"); sys.exit(0)
print(f"RETIRED HISTORY in {ref} ({bad[0][:12]}); reverting the push")
if set(before) == {"0"}: subprocess.run(["git", "push", "origin", f":{ref}"], check=True)
else: subprocess.run(["git", "push", "--force", "origin", f"{before}:{ref}"], check=True)
sys.exit(1)
