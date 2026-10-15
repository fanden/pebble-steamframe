from frame_agent import acf_value, parse_volume

assert parse_volume("Volume: 0.45 [MUTED]\n") == (45, True)
assert parse_volume("Volume: 1.00\n") == (100, False)
assert parse_volume("") == (0, False)

acf = '"AppState"\n{\n\t"appid"\t\t"620"\n\t"name"\t\t"Portal 2"\n\t"LastPlayed"\t\t"1760000000"\n}'
assert acf_value(acf, "appid") == "620"
assert acf_value(acf, "name") == "Portal 2"
assert acf_value(acf, "missing") == ""


# pairing hands out the token exactly once
import tempfile
from pathlib import Path
import frame_agent
frame_agent.TOKEN_FILE = Path(tempfile.mkdtemp()) / "sub/token"
t = frame_agent.pair()
assert t and frame_agent.token() == t
assert frame_agent.pair() is None
assert oct(frame_agent.TOKEN_FILE.stat().st_mode & 0o777) == "0o600"
print("pair ok")
