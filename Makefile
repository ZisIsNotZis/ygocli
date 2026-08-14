# ygocli is now the MCP-bridged ygopro client fork built at /home/z/ygo.
# This Makefile only drives the network test.

.PHONY: test-net

test-net:
	python3 -u tests/fork_net_test.py
