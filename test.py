#!/usr/bin/python3
# A program interleaving stdout and stderr output.
# The output ordering when invoked directly and under ./prun should match.
import sys
sys.stdout.write("1\n")
sys.stdout.write("2\n")
sys.stdout.write("3\n")
sys.stderr.write("4")
sys.stdout.write("5\n")
sys.stderr.write("\n")
