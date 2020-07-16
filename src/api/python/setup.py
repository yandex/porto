from setuptools import setup
import re
import os
import sys

def version():
    if os.path.exists("PKG-INFO"):
        with open("PKG-INFO") as f:
            return re.search(r'\nVersion: (\S+)\n', f.read()).group(1)
    if os.path.exists("../../../debian/changelog"):
        with open("../../../debian/changelog") as f:
            return re.search(r'.*\((.*)\).*', f.readline()).group(1)
    return "0.0.0"

def readme():
    with open('README.rst') as f:
        return f.read()

if __name__ == '__main__':

    if not os.path.exists("porto/rpc.proto"):
        with open("../../rpc.proto", 'r') as src, open("porto/rpc.proto", 'w') as dst:
            dst.write(src.read())

    try:
        import subprocess
        subprocess.check_call(['protoc', '--python_out=porto', '--proto_path=porto', 'porto/rpc.proto'])
    except Exception as e:
        sys.stderr.write("Cannot compile rpc.proto: {}\n".format(e))

    if not os.path.exists("porto/rpc_pb2.py"):
        sys.stderr.write("Compiled rpc.proto not found\n")
        sys.exit(-1)

    setup(name='portopy',
          version=version(),
          description='Python API for porto',
          long_description=readme(),
          url='https://github.com/yandex/porto',
          author_email='max7255@yandex-team.ru',
          maintainer_email='max7255@yandex-team.ru',
          license='GNU LGPL v3 License',
          packages=['porto'],
          package_data={'porto': ['rpc.proto']},
          install_requires=['protobuf'],
          zip_safe=False)
