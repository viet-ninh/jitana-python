#include <python/Python.h>
#include <stdio.h>
#include <iostream>
#include <fstream>

#include <boost/graph/graphviz.hpp>

char script_name[256];
char func_name[256];

// Load Python module with a given name
PyObject* load_python_module(const char* module_name){

    PyObject *pName = PyUnicode_FromString(module_name);
    PyObject *pModule = PyImport_Import(pName);
    Py_DECREF(pName);
    if (!pModule) {
        PyErr_Print();
        fprintf(stderr, "Failed to load module\n");
    }
    return pModule; 
}

// Load a Python function from a module
PyObject* load_python_function(PyObject *pModule, const char* func_name) {
    PyObject *pFunc = PyObject_GetAttrString(pModule, func_name);
    if (!pFunc || !PyCallable_Check(pFunc)) {
        PyErr_Print();
        fprintf(stderr, "Function %s not found or not callable\n", func_name);
        Py_XDECREF(pFunc);
        return NULL;
    }
    return pFunc;
}

// Print the bytecode of a function
void print_bytecode(PyObject *pFunc) {
    PyObject *pCode = PyObject_GetAttrString(pFunc, "__code__");
    if (pCode) {
        PyObject *pBytecode = PyObject_GetAttrString(pCode, "co_code");
        if (pBytecode && PyBytes_Check(pBytecode)) {
            char *bytecode = PyBytes_AsString(pBytecode);
            Py_ssize_t bytecode_len = PyBytes_Size(pBytecode);
            std::ofstream file("output/bytecode.txt");
            if (file.is_open()){
                
                file << "Bytecode for function: \n";
                for (Py_ssize_t i = 0; i < bytecode_len; i++) {
                    file << std::hex << std::setw(2) << std::setfill('0') 
                         << static_cast<unsigned int>(bytecode[i] & 0xFF) << " ";
                }
                file.close();
            }
            else{
                std::cerr << "Error: Unable to create file. \n";
            }

            Py_DECREF(pBytecode);
        }
        Py_DECREF(pCode);
    }
}

void disassemble_function(PyObject *pFunc) {
    PyObject *pCode = PyObject_GetAttrString(pFunc, "__code__");
    PyObject *dis_module = PyImport_ImportModule("dis");

    if (pCode && dis_module) {
        // Get dis.Bytecode from the dis module
        PyObject *bytecode_class = PyObject_GetAttrString(dis_module, "Bytecode");

        if (bytecode_class && PyCallable_Check(bytecode_class)) {
            // Create a Bytecode object
            PyObject *args = PyTuple_Pack(1, pCode);
            PyObject *bytecode_obj = PyObject_CallObject(bytecode_class, args);
            Py_DECREF(args);

            if (bytecode_obj) {
                std::ofstream file("output/code_instructions.txt");
                if (file.is_open()) {
                    file << "Bytecode for function:\n";

                    // Iterate over Bytecode object (it is an iterable and contains instructions of the program)
                    PyObject *iterator = PyObject_GetIter(bytecode_obj);
                    PyObject *instr;
                    while ((instr = PyIter_Next(iterator))) {
                        PyObject *repr = PyObject_Repr(instr);
                        const char *instr_str = PyUnicode_AsUTF8(repr);
                        file << instr_str << "\n";
                        Py_DECREF(repr);
                        Py_DECREF(instr);
                    }
                    Py_DECREF(iterator);
                    file.close();
                } else {
                    std::cerr << "Error: Unable to create file.\n";
                }

                Py_DECREF(bytecode_obj);
            } else {
                PyErr_Print();
                fprintf(stderr, "Failed to create Bytecode object\n");
            }

            Py_DECREF(bytecode_class);
        } else {
            PyErr_Print();
            fprintf(stderr, "Failed to find or call dis.Bytecode()\n");
        }

        Py_DECREF(dis_module);
        Py_DECREF(pCode);
    } else {
        PyErr_Print();
        fprintf(stderr, "Failed to retrieve function code object or import dis module\n");
    }
}


int main(int argc, char* argv[]) {
    std::string file_path = argv[1];
    Py_Initialize();

    std::string command = 
        "import sys\n"
        "sys.path.append('.')\n"
        "sys.path.append('" + file_path + "')\n";

    PyRun_SimpleString(command.c_str());

    // Get module and function name from user
    printf("Input script name: ");
    scanf("%255s", script_name);
    PyObject *pModule = load_python_module(script_name);
    if (!pModule) return 1;

    printf("Input the script's function name: ");
    scanf("%255s", func_name);
    PyObject *pFunc = load_python_function(pModule, func_name);
    if (!pFunc) {
        Py_DECREF(pModule);
        return 1;
    }

    print_bytecode(pFunc);
    disassemble_function(pFunc);

    Py_DECREF(pModule);
    Py_DECREF(pFunc);
    Py_Finalize();
    return 0;
}