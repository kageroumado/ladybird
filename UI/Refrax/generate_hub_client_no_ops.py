#!/usr/bin/env python3
# Writes an empty override for each message of the HubClient endpoint, from the header that the IPC compiler makes.

import re
import sys

endpoint_header, output = sys.argv[1], sys.argv[2]

handler = re.compile(r"^\s*virtual void (\w+)\((.*)\) = 0;\s*$")


def split_parameters(parameters):
    depth = 0
    start = 0
    for index, character in enumerate(parameters):
        if character in "<(":
            depth += 1
        elif character in ">)":
            depth -= 1
        elif character == "," and depth == 0:
            yield parameters[start:index].strip()
            start = index + 1
    if parameters[start:].strip():
        yield parameters[start:].strip()


# An empty handler uses none of its parameters, so they lose their names.
def without_name(parameter):
    return re.sub(r"\s*\b\w+$", "", parameter)


handlers = []
in_stub = False
with open(endpoint_header) as header:
    for line in header:
        if line.startswith("class HubClientStub"):
            in_stub = True
        elif in_stub and (match := handler.match(line)):
            handlers.append((match[1], ", ".join(without_name(parameter) for parameter in split_parameters(match[2]))))

if not handlers:
    sys.exit(f"{endpoint_header} declares no HubClient handlers")

with open(output, "w") as out:
    for name, parameters in handlers:
        out.write(f"virtual void {name}({parameters}) override {{ }}\n")
