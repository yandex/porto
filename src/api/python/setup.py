from setuptools import setup

def readme():
    with open('README.rst') as f:
        return f.read()

setup(name='portopy',
    version='2.7.7',
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

