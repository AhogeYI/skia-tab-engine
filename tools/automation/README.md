# TabEngine automation tools

`te.py` is the standard-library Python client and CLI for the TabEngine
automation protocol. It talks to a running application over the loopback
HTTP transport and requires nothing beyond Python 3.10+; it is not a
runtime dependency of any product.

## Usage

```bash
# The host publishes a connection descriptor and a token separately:
python te.py --descriptor ./automation-descriptor.json --token-file ./automation-token.txt info
TABENGINE_TOKEN=<token> python te.py --endpoint http://127.0.0.1:53123 windows

python te.py --descriptor ... snapshot 1                    # semantic tree
python te.py --descriptor ... click 1 --key runTask         # click by stable key
python te.py --descriptor ... type 1 "hello"
python te.py --descriptor ... wait activityQuiet --scope tasks
python te.py --descriptor ... screenshot 1 --out shot.png
python te.py --descriptor ... logs --source app
python te.py --descriptor ... call app.myapp.getState
```

Run `python te.py --help` for every command. All commands accept `--json`
for raw results; RPC failures exit non-zero with the error on stderr.

See `docs/AUTOMATION_GUIDE.md` in the SDK for the full protocol, security
posture and extension contracts.
