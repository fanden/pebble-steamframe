from frame_agent import acf_value, parse_volume

assert parse_volume("Volume: 0.45 [MUTED]\n") == (45, True)
assert parse_volume("Volume: 1.00\n") == (100, False)
assert parse_volume("") == (0, False)

acf = '"AppState"\n{\n\t"appid"\t\t"620"\n\t"name"\t\t"Portal 2"\n\t"LastPlayed"\t\t"1760000000"\n}'
assert acf_value(acf, "appid") == "620"
assert acf_value(acf, "name") == "Portal 2"
assert acf_value(acf, "missing") == ""
print("ok")
