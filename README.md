<p id="title" align="center">
  <a href="#title">
    <img width="240" src="https://i.imgur.com/G6R78PP.png">
    <h1 align="center">Yde Programming Language</h1>
  </a>
</p>

<p align="center">

  <a aria-label="Made By Edvaldo" href="https://github.com/edvaldinhs/">
    <img src="https://img.shields.io/badge/MADE%20BY%20Edvaldo-000000.svg?style=for-the-badge&labelColor=000&logo=starship&logoColor=fff&logoWidth=20">
  </a>
</p>

<p align="center">A compiler project for my own Programming Language</p>

<br>

## 🧪&nbsp; Technologies

This project was developed with the following technologies:

- [C++](https://isocpp.org/)
- [LLVM](https://llvm.org/)
- [CMAKE](https://cmake.org/)
- [CLANG](https://clang.org/)

<br>

## 🎲&nbsp; About

A custom-built compiler for an original programming language currently in development. The language is designed to offer the power of C and C++ through a streamlined syntax that prioritizes developer efficiency.

<br>

## 🧑🏻‍💻&nbsp; Getting Started

You will need to download llvm libs before building the project...

### Ubuntu
```bash
$ sudo apt install llvm-dev libclang-dev
```

### Arch
```bash
$ sudo pacman -S llvm
```

### Build
Clone the project and access the project folder:

```bash
$ git clone https://github.com/edvaldinhs/yde-language
$ cd yde-language
```

You can use the setup.sh to build:

```bash
$ ./setup.sh
$ ./yde ../examples/for.yde
```

Or build the project manually:

```bash
$ mkdir build
$ cd build
$ cmake ..
$ make
$ ./yde ../examples/for.yde
```

You can create your own .yde files to test it out!

<br>

## 💻&nbsp; Project

Building a programming language from scratch to master the inner workings of compilers. This project covers the entire pipeline: designing a custom syntax, building a robust parser, and leveraging LLVM for high-performance execution.

<br>

## 🧑&nbsp; Authors

<p align="center">
    <img width="20%" src="https://github.com/edvaldinhs.png" alt="Edvaldo Henrique">
  <p align="center">
    Edvaldo Henrique
  </p >
</p>
