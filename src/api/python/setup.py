from setuptools import setup

def readme():
    with open('README.rst') as f:
        return f.read()

setup(name='portopy',
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
    use_scm_version={
        'root': '../../..',
        'version_scheme': 'post-release',
        'local_scheme': 'node-and-date',
    },
    setup_requires=['setuptools_scm'],
    zip_safe=False)

