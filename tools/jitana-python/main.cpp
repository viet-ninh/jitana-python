#include <python/Python.h>
#include <stdio.h>

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
    // PyObject *pCode = PyObject_GetAttrString(pFunc, "__class__");
    PyObject *pCode = PyObject_GetAttrString(pFunc, "__code__");
    if (pCode) {
        PyObject *pBytecode = PyObject_GetAttrString(pCode, "co_code");
        if (pBytecode && PyBytes_Check(pBytecode)) {
            char *bytecode = PyBytes_AsString(pBytecode);
            Py_ssize_t bytecode_len = PyBytes_Size(pBytecode);

            printf("Bytecode for the function:\n");
            for (Py_ssize_t i = 0; i < bytecode_len; i++) {
                printf("%02x ", (unsigned char) bytecode[i]);
            }
            printf("\n");
            Py_DECREF(pBytecode);
        }
        Py_DECREF(pCode);
    }
}

int main() {
    Py_Initialize();
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

    Py_DECREF(pModule);
    Py_DECREF(pFunc);
    Py_Finalize();
    return 0;
}