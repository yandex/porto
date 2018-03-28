from setuptools import setup
import re
import os

def version():
    if os.path.exists('PKG-INFO'):
        with open("PKG-INFO") as f:
            return re.search(r'\nVersion: (\S+)\n', f.read()).group(1)
    with open("../../../debian/changelog") as f:
        return re.search(r'.*\((.*)\).*', f.readline()).group(1)

def readme():
    with open('README.rst') as f:
        return f.read()

setup(name='portopy',
    version=version(),
    description='Python API for porto',
    long_description=readme(),
    url='https://github.com/yandex/porto',
    author='marchael',
    author_email='marchael@yandex-team.ru',
    license='none',
    packages=['porto'],
    install_requires=[
        'protobuf',
    ],
    zip_safe=False)

