#include <python/Python.h>
#include <stdio.h>
#include <iostream>
#include <fstream>

#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/graphviz.hpp>
#include <unordered_map>

char script_name[256];
char func_name[256];

// Define a graph with a vertex property for labels
struct VertexProperties {
    std::string label;
};

// Define a directed graph using Boost
using Graph = boost::adjacency_list<boost::vecS, boost::vecS, boost::directedS, VertexProperties>;

// Map function names to vertex descriptors
using Vertex = boost::graph_traits<Graph>::vertex_descriptor;
using VertexMap = std::unordered_map<std::string, Vertex>;

struct Instruction {
    std::string opname;
    int opcode;
    int arg;
    std::string argval;
};

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

std::vector<Instruction> disassemble_function(PyObject *pFunc) {
    std::vector<Instruction> instructions;

    // Try to get the __code__ object directly
    PyObject *pCode = PyObject_GetAttrString(pFunc, "__code__");

    // If not found, try __func__.__code__ (e.g., for bound methods)
    if (!pCode) {
        PyErr_Clear();  // Clear error from failed __code__ access

        PyObject *pFuncAttr = PyObject_GetAttrString(pFunc, "__func__");
        if (pFuncAttr) {
            pCode = PyObject_GetAttrString(pFuncAttr, "__code__");
            Py_DECREF(pFuncAttr);
        }
    }

    // If still no code object, give up (likely a built-in or C function)
    if (!pCode) {
        std::cerr << "Cannot disassemble: no __code__ object found (likely a built-in or C extension function)\n";
        PyErr_Clear();  // Clear any error
        return instructions;
    }

    // Import dis module
    PyObject *dis_module = PyImport_ImportModule("dis");
    if (!dis_module) {
        std::cerr << "Failed to import 'dis' module\n";
        Py_DECREF(pCode);
        PyErr_Print();
        return instructions;
    }

    // Get dis.Bytecode class
    PyObject *bytecode_class = PyObject_GetAttrString(dis_module, "Bytecode");
    if (!bytecode_class || !PyCallable_Check(bytecode_class)) {
        std::cerr << "Failed to access or call dis.Bytecode\n";
        Py_XDECREF(bytecode_class);
        Py_DECREF(dis_module);
        Py_DECREF(pCode);
        PyErr_Print();
        return instructions;
    }

    // Create Bytecode object
    PyObject *args = PyTuple_Pack(1, pCode);
    PyObject *bytecode_obj = PyObject_CallObject(bytecode_class, args);
    Py_DECREF(args);
    Py_DECREF(bytecode_class);
    Py_DECREF(dis_module);
    Py_DECREF(pCode);

    if (!bytecode_obj) {
        std::cerr << "Failed to create Bytecode object\n";
        PyErr_Print();
        return instructions;
    }

    // Iterate through bytecode instructions
    PyObject *iterator = PyObject_GetIter(bytecode_obj);
    Py_DECREF(bytecode_obj);

    if (!iterator) {
        std::cerr << "Failed to get iterator from Bytecode object\n";
        PyErr_Print();
        return instructions;
    }

    PyObject *instr;
    while ((instr = PyIter_Next(iterator))) {
        Instruction inst{};

        // Get opname
        PyObject *opname_obj = PyObject_GetAttrString(instr, "opname");
        if (opname_obj) {
            inst.opname = PyUnicode_AsUTF8(opname_obj);
            Py_DECREF(opname_obj);
        }

        // Get opcode
        PyObject *opcode_obj = PyObject_GetAttrString(instr, "opcode");
        if (opcode_obj) {
            inst.opcode = PyLong_AsLong(opcode_obj);
            Py_DECREF(opcode_obj);
        }

        // Get arg
        PyObject *arg_obj = PyObject_GetAttrString(instr, "arg");
        if (arg_obj && PyLong_Check(arg_obj)) {
            inst.arg = PyLong_AsLong(arg_obj);
            Py_DECREF(arg_obj);
        } else {
            inst.arg = -1;
        }

        // Get argval
        PyObject *argval_obj = PyObject_GetAttrString(instr, "argval");
        if (argval_obj) {
            if (PyUnicode_Check(argval_obj)) {
                inst.argval = PyUnicode_AsUTF8(argval_obj);
            } else if (PyLong_Check(argval_obj)) {
                inst.argval = std::to_string(PyLong_AsLong(argval_obj));
            } else if (argval_obj == Py_None) {
                inst.argval = "None";
            } else {
                PyObject *repr = PyObject_Repr(argval_obj);
                if (repr) {
                    inst.argval = PyUnicode_AsUTF8(repr);
                    Py_DECREF(repr);
                } else {
                    inst.argval = "<unknown>";
                }
            }
            Py_DECREF(argval_obj);
        } else {
            inst.argval = "";
        }

        instructions.push_back(inst);
        Py_DECREF(instr);
    }

    Py_DECREF(iterator);
    return instructions;
}


void write_instructions_to_file(const std::vector<Instruction>& instructions, const std::string& filename) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Unable to create file.\n";
        return;
    }

    file << "Bytecode Instructions:\n";
    for (const auto &inst : instructions) {
        file << inst.opname << " (" << inst.opcode << ")";
        if (inst.arg != -1) {
            file << " Arg: " << inst.arg << " ArgVal: " << inst.argval;
        }
        file << "\n";
    }
    file.close();
}

void build_function_call_graph(Graph &graph, VertexMap &vertex_map, 
                               const std::vector<Instruction> &instructions, 
                               const std::string &function_name) {
    // Add the main function as a vertex
    Vertex main_vertex;
    if (vertex_map.find(function_name) == vertex_map.end()) {
        main_vertex = boost::add_vertex(graph);
        vertex_map[function_name] = main_vertex;
        graph[main_vertex].label = function_name;
    } else {
        main_vertex = vertex_map[function_name];
    }

    // Iterate over instructions to find function calls
    std::string last_global; // Track last LOAD_GLOBAL value for method calls

    for (size_t i = 0; i < instructions.size(); ++i) {
        const auto &inst = instructions[i];

        if (inst.opname == "LOAD_GLOBAL") {
            last_global = inst.argval; // Save global variable name (e.g., "np")
        } 

        else if (inst.opname == "LOAD_ATTR" && !last_global.empty()) {
            // If LOAD_ATTR follows LOAD_GLOBAL, construct full method name (e.g., "np.sum")
            last_global += "." + inst.argval;  
        } 

        else if (inst.opname == "CALL_FUNCTION" || inst.opname == "CALL_METHOD" || inst.opname == "CALL") {
            std::string called_function;

            // If we just processed a method call, use last_global (e.g., "np.sum")
            if (!last_global.empty()) {
                called_function = last_global;
                last_global.clear(); // Reset tracking for the next iteration
            } else {
                called_function = inst.argval;
            }

            if (!called_function.empty()) {
                // Ensure the function exists as a node
                Vertex called_vertex;
                if (vertex_map.find(called_function) == vertex_map.end()) {
                    called_vertex = boost::add_vertex(graph);
                    vertex_map[called_function] = called_vertex;
                    graph[called_vertex].label = called_function;
                } else {
                    called_vertex = vertex_map[called_function];
                }

                // Add an edge from the caller to the callee
                boost::add_edge(main_vertex, called_vertex, graph);
            }
        }
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

    // print_bytecode(pFunc);
    std::vector<Instruction> instructions = disassemble_function(pFunc);
    write_instructions_to_file(instructions, "output/function_instructions.txt");

    // Create the Boost Graph and a mapping of function names to nodes
    Graph function_call_graph;
    VertexMap vertex_map;

    // Build the function call graph
    build_function_call_graph(function_call_graph, vertex_map, instructions, func_name);

    // (Optional) Output the graph
    std::ofstream dot_file("output/function_calls.dot");
    boost::write_graphviz(dot_file, function_call_graph,
        boost::make_label_writer(boost::get(&VertexProperties::label, function_call_graph))); 
    dot_file.close();

    Py_DECREF(pModule);
    Py_DECREF(pFunc);
    Py_Finalize();
    return 0;
}