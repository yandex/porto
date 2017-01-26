import porto, sys

l = []
for i in range(int(sys.argv[1])):
    c = porto.Connection();
    c.connect();
    l.append(c)

sys.stdin.read()
