from setuptools import setup

def readme():
    with open('README.rst') as f:
        return f.read()

setup(name='portopy',
    version='0.1',
    description='Python API for porto',
    long_description=readme(),
    url='https://git.yandex.ru/gitweb/search-admin/porto.git',
    author='marchael',
    author_email='marchael@yandex-team.ru',
    license='none',
    packages=['porto'],
    install_requires=[
        'protobuf',
    ],
    zip_safe=False)

