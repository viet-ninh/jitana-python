#include <python/Python.h>
#include <stdio.h>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <set>


#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/graphviz.hpp>
#include <unordered_map>

char script_name[256];
char func_name[256];
char class_name[256];

// Define a graph with a vertex property for labels
struct VertexProperties {
    std::string label;
};

struct EdgeProperties{
    std:: string label;
};

// Define a directed graph using Boost
using Graph = boost::adjacency_list<boost::vecS, boost::vecS, boost::directedS, VertexProperties, EdgeProperties>;

// Map function names to vertex descriptors
using Vertex = boost::graph_traits<Graph>::vertex_descriptor;
using VertexMap = std::unordered_map<std::string, Vertex>;

struct Instruction {
    std::string opname;
    int opcode;
    int arg;
    std::string argval;
    std::string source_label;
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

PyObject* load_class_function(PyObject* pModule, const char* class_name, const char* method_name) {
    PyObject* pClass = PyObject_GetAttrString(pModule, class_name);
    if (!pClass || !PyType_Check(pClass)) {
        PyErr_Print();
        std::cerr << "Class " << class_name << " not found or not a class.\n";
        Py_XDECREF(pClass);
        return nullptr;
    }

    PyObject* pFunc = PyObject_GetAttrString(pClass, method_name);
    Py_DECREF(pClass);

    if (!pFunc || !PyCallable_Check(pFunc)) {
        PyErr_Print();
        std::cerr << "Method " << method_name << " not found or not callable.\n";
        Py_XDECREF(pFunc);
        return nullptr;
    }

    return pFunc;
}

void find_classes_and_functions(PyObject* pModule, std::vector<std::string>& classes, std::vector<std::string>& functions) {
    PyObject* attrs = PyObject_Dir(pModule);
    Py_ssize_t len = PyList_Size(attrs);

    for (Py_ssize_t i = 0; i < len; ++i) {
        PyObject* attrName = PyList_GetItem(attrs, i);  // borrowed
        const char* attrStr = PyUnicode_AsUTF8(attrName);
        PyObject* attr = PyObject_GetAttr(pModule, attrName);

        if (attr) {
            if (PyType_Check(attr)) {
                classes.push_back(attrStr);
            } else if (PyFunction_Check(attr)) {
                functions.push_back(attrStr);
            }
            Py_DECREF(attr);
        }
    }
    Py_DECREF(attrs);
}

void list_class_methods(PyObject* pClass, std::vector<std::string>& methods) {
    PyObject* dirList = PyObject_Dir(pClass);
    Py_ssize_t len = PyList_Size(dirList);

    for (Py_ssize_t i = 0; i < len; ++i) {
        PyObject* attrName = PyList_GetItem(dirList, i);  // borrowed
        const char* attrStr = PyUnicode_AsUTF8(attrName);
        PyObject* attr = PyObject_GetAttr(pClass, attrName);  // new reference

        if (attr && PyCallable_Check(attr)) {
            methods.push_back(attrStr);
        }

        Py_XDECREF(attr);
    }

    Py_DECREF(dirList);
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

    PyObject* instr;
    while ((instr = PyIter_Next(iterator))) {
        Instruction inst{};

        // Get opname (the name of the bytecode instruction, e.g., 'LOAD_FAST', 'CALL_FUNCTION')
        PyObject* opname_obj = PyObject_GetAttrString(instr, "opname");
        if (opname_obj) {
            inst.opname = PyUnicode_AsUTF8(opname_obj);
            Py_DECREF(opname_obj);
        }

        // Get opcode (the opcode value, e.g., 100, 101)
        PyObject* opcode_obj = PyObject_GetAttrString(instr, "opcode");
        if (opcode_obj) {
            inst.opcode = PyLong_AsLong(opcode_obj);
            Py_DECREF(opcode_obj);
        }

        // Get argument (if available, this can vary depending on the opcode)
        PyObject* arg_obj = PyObject_GetAttrString(instr, "arg");
        if (arg_obj && PyLong_Check(arg_obj)) {
            inst.arg = PyLong_AsLong(arg_obj);
            Py_DECREF(arg_obj);
        } else {
            inst.arg = -1; // Default value for instructions with no argument
        }

        // Get argument value (this can be a string, integer, None, or more complex object)
        PyObject* argval_obj = PyObject_GetAttrString(instr, "argval");
        if (argval_obj) {
            if (PyUnicode_Check(argval_obj)) {
                inst.argval = PyUnicode_AsUTF8(argval_obj); // If argument is a string
            } else if (PyLong_Check(argval_obj)) {
                inst.argval = std::to_string(PyLong_AsLong(argval_obj)); // If argument is an integer
            } else if (argval_obj == Py_None) {
                inst.argval = "None"; // Handle None type
            } else {
                // If argument is of unknown type, get the string representation
                PyObject* repr = PyObject_Repr(argval_obj);
                if (repr) {
                    inst.argval = PyUnicode_AsUTF8(repr);
                    Py_DECREF(repr);
                } else {
                    inst.argval = "<unknown>"; // Default for unknown types
                }
            }
            Py_DECREF(argval_obj);
        } else {
            inst.argval = ""; // Default if no argument value is found
        }

        // Add the processed instruction to the list
        instructions.push_back(inst);
        Py_DECREF(instr); // Clean up the current instruction object
    }

    Py_DECREF(iterator);
    return instructions;
}

// Utility to scan backward to find LOAD_GLOBAL (and possibly LOAD_ATTR)
int find_global_load(const std::vector<Instruction>& instructions, int call_index, std::string& func_name, std::string& full_name) {
    // Look back up to 5 instructions
    for (int j = call_index - 1; j >= 0 && call_index - j <= 5; --j) {
        if (instructions[j].opname == "LOAD_GLOBAL") {
            func_name = instructions[j].argval;
            full_name = func_name;  // default
            if (j + 1 < call_index && instructions[j + 1].opname == "LOAD_ATTR") {
                full_name += "." + instructions[j + 1].argval;
            }
            return j;
        }
    }
    return -1;
}

void disassemble_called_functions_recursive(
    PyObject* context_function,
    const std::vector<Instruction>& instructions,
    std::map<std::string, std::vector<Instruction>>& called_functions,
    std::set<std::string>& visited
) {
    PyObject* globals_dict = PyObject_GetAttrString(context_function, "__globals__");
    if (!globals_dict) {
        std::cerr << "Could not get globals from function.\n";
        PyErr_Print();
        return;
    }

    for (size_t i = 0; i < instructions.size(); ++i) {
        const Instruction& inst = instructions[i];
        if (inst.opname == "CALL" || inst.opname == "CALL_FUNCTION") {
            std::string func_name, full_name;
            int load_index = find_global_load(instructions, i, func_name, full_name);
            if (load_index >= 0 && visited.count(full_name) == 0) {
                PyObject* target = nullptr;

                PyObject* module_or_func = PyDict_GetItemString(globals_dict, func_name.c_str());
                if (module_or_func) {
                    if (full_name != func_name) { // Has attribute, like np.sum
                        std::string attr = full_name.substr(func_name.length() + 1);
                        target = PyObject_GetAttrString(module_or_func, attr.c_str());
                    } else {
                        target = module_or_func;
                        Py_INCREF(target);
                    }

                    if (target && PyCallable_Check(target)) {
                        std::vector<Instruction> inner = disassemble_function(target);
                        if (!inner.empty()) {
                            called_functions[full_name] = inner;
                            visited.insert(full_name);
                            disassemble_called_functions_recursive(target, inner, called_functions, visited);
                        }
                        Py_DECREF(target);
                    }
                }
            }
        }

    }

    Py_DECREF(globals_dict);
}


std::map<std::string, std::vector<Instruction>> disassemble_all_called_functions(PyObject* original_function, const char* func_name) {
    std::map<std::string, std::vector<Instruction>> called_functions;
    std::set<std::string> visited;

    std::vector<Instruction> top_level = disassemble_function(original_function);
    called_functions[func_name] = top_level;
    disassemble_called_functions_recursive(original_function, top_level, called_functions, visited);
    std::cout << "Disassembled functions:\n";
    for (const auto& entry : called_functions) {
        std::cout << "Function: " << entry.first << "\n";
    }
    return called_functions;
}

void write_instructions_to_file(
    const std::map<std::string, std::vector<Instruction>>& instruction_map,
    const std::string& output_dir
) {
    // Create the directory if it doesn't exist
    if (!std::filesystem::exists(output_dir)) {
        if (!std::filesystem::create_directories(output_dir)) {
            std::cerr << "Error: Unable to create output directory.\n";
            return;
        }
    }

    for (const auto& [func_name, instructions] : instruction_map) {
        // Sanitize filename (replace '.' with '_', etc.)
        std::string safe_name = func_name;
        std::replace(safe_name.begin(), safe_name.end(), '.', '_');
        std::string filename = output_dir + "/" + safe_name + ".txt";

        std::ofstream file(filename);
        if (!file.is_open()) {
            std::cerr << "Error: Unable to write file for function: " << func_name << "\n";
            continue;
        }

        file << "Bytecode Instructions for function: " << func_name << "\n";
        for (const auto& inst : instructions) {
            file << inst.opname << " (" << inst.opcode << ")";
            if (inst.arg != -1) {
                file << " Arg: " << inst.arg << " ArgVal: " << inst.argval;
            }
            file << "\n";
        }

        file.close();
    }
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
std::vector<std::string> callable_stack;

for (size_t i = 0; i < instructions.size(); ++i) {
    const auto &inst = instructions[i];

    if (inst.opname == "LOAD_GLOBAL") {
        callable_stack.push_back(inst.argval);  // e.g., "sqrtm"
    }
    else if (inst.opname == "CALL" || inst.opname == "CALL_FUNCTION" || inst.opname == "CALL_METHOD" || inst.opname == "CALL_KW") {
        if (!callable_stack.empty()) {
            std::string called_function = callable_stack.back();
            callable_stack.pop_back();

            Vertex called_vertex;
            if (vertex_map.find(called_function) == vertex_map.end()) {
                called_vertex = boost::add_vertex(graph);
                vertex_map[called_function] = called_vertex;
                graph[called_vertex].label = called_function;
            } else {
                called_vertex = vertex_map[called_function];
            }

            boost::add_edge(main_vertex, called_vertex, graph);
        }
    }

    // Optional: Clear if unrelated op
    // else if (...) { callable_stack.clear(); }
}

}

std::map<std::string, std::shared_ptr<Graph>> write_function_call_graphs_to_dot(
    const std::string& output_dir,
    const std::map<std::string, std::vector<Instruction>>& instruction_map
) {
    namespace fs = std::filesystem;

    // Create the directory if it doesn't exist
    if (!fs::exists(output_dir)) {
        if (!fs::create_directories(output_dir)) {
            std::cerr << "Error: Unable to create output directory: " << output_dir << "\n";
        }
    }
    std::map<std::string, std::shared_ptr<Graph>> graph_map;

    for (const auto& [func_name, instructions] : instruction_map) {
        auto subgraph = std::make_shared<Graph>();
        VertexMap sub_vertex_map;

        build_function_call_graph(*subgraph, sub_vertex_map, instructions, func_name);

        graph_map[func_name] = subgraph; // store the subgraph

        // Create filename
        std::string safe_func_name = func_name;
        std::replace(safe_func_name.begin(), safe_func_name.end(), '.', '_');
        std::string filename = output_dir + "/" + safe_func_name + "_calls.dot";

        std::ofstream dot_file(filename);
        if (!dot_file) {
            std::cerr << "Error: Could not write to file " << filename << "\n";
            continue;
        }

        boost::write_graphviz(dot_file, *subgraph,
            boost::make_label_writer(boost::get(&VertexProperties::label, *subgraph)));
        dot_file.close();
    }
    return graph_map;
}

std::string resolve_called_function(const Instruction& instr) {
    return instr.argval;
}


Graph combine_all_function_graphs(
    const std::map<std::string, std::shared_ptr<Graph>>& graph_map,
    const std::string& output_file
) {
    Graph master_graph;

    // NEW: Track label-to-vertex mapping for inter-graph links
    // STEP 1: Copy subgraphs and deduplicate by label
    std::unordered_map<std::string, Vertex> label_to_vertex;

    for (const auto& [func_name, subgraph_ptr] : graph_map) {
        const Graph& subgraph = *subgraph_ptr;
        std::unordered_map<Vertex, Vertex> vertex_map;

        for (auto [vi, vi_end] = boost::vertices(subgraph); vi != vi_end; ++vi) {
            const std::string& label = subgraph[*vi].label;

            Vertex new_v;
            if (label_to_vertex.find(label) == label_to_vertex.end()) {
                new_v = boost::add_vertex(master_graph);
                master_graph[new_v].label = label;
                label_to_vertex[label] = new_v;
            } else {
                new_v = label_to_vertex[label];
            }

            vertex_map[*vi] = new_v;
        }

        for (auto [ei, ei_end] = boost::edges(subgraph); ei != ei_end; ++ei) {
            Vertex src = boost::source(*ei, subgraph);
            Vertex tgt = boost::target(*ei, subgraph);
            boost::add_edge(vertex_map[src], vertex_map[tgt], master_graph);
        }
    }


    // STEP 3: Write to DOT file
    std::ofstream dot_file(output_file);
    if (!dot_file) {
        std::cerr << "Error: could not open file for writing: " << output_file << "\n";
    } else {
        boost::write_graphviz(dot_file,
            master_graph,
            boost::make_label_writer(boost::get(&VertexProperties::label, master_graph)));
        dot_file.close();
    }

    return master_graph;
}


int main(int argc, char* argv[]) {
    if (argc > 1){
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
        if (!pModule){
            Py_Finalize();
            return 1;
        }
        std::vector<std::string> classes;
        std::vector<std::string> functions;
        find_classes_and_functions(pModule, classes, functions);


        std::cout << "\nAvailable classes:\n";
        for (const auto& cls : classes) std::cout << "  - " << cls << "\n";
        std::cout << "Available functions:\n";
        for (const auto& func : functions) std::cout << "  - " << func << "\n";

        std::string choice;
        std::cout << "\nAnalyze a [class] method or [function]? ";
        std::cin >> choice;

        PyObject* pFunc = nullptr;

        if (choice == "function") {
            std::cout << "Enter function name: ";
            std::cin >> func_name;
            pFunc = load_python_function(pModule, func_name);
        } else if (choice == "class") {
            std::cout << "Enter class name: ";
            std::cin >> class_name;

            PyObject* pClass = PyObject_GetAttrString(pModule, class_name);
            if (!pClass || !PyType_Check(pClass)) {
                PyErr_Print();
                std::cerr << "Invalid class.\n";
                Py_XDECREF(pClass);
            } else {
                std::vector<std::string> class_methods;
                list_class_methods(pClass, class_methods);

                std::cout << "Methods in class " << class_name << ":\n";
                for (const auto& method : class_methods)
                    std::cout << "  - " << method << "\n";

                std::cout << "Enter method name: ";
                std::cin >> func_name;

                pFunc = PyObject_GetAttrString(pClass, func_name);
                if (!pFunc || !PyCallable_Check(pFunc)) {
                    PyErr_Print();
                    std::cerr << "Method not found or not callable.\n";
                    Py_XDECREF(pFunc);
                }

                Py_DECREF(pClass);
            }
    } else {
        std::cerr << "Invalid choice.\n";
    }

        if (pFunc){
            // Create a dictionary of instructions where each function name is the key to its bytecode instructions
            std::map<std::string, std::vector<Instruction>> instruction_map = disassemble_all_called_functions(pFunc, func_name);
            write_instructions_to_file(instruction_map, "output/bytecodes");

            std::map<std::string, std::shared_ptr<Graph>> graph_map = write_function_call_graphs_to_dot("output/graphs", instruction_map);

            Graph master_graph = combine_all_function_graphs(graph_map, "output/master_graph.dot");

            Py_DECREF(pModule);
            Py_DECREF(pFunc);
            Py_Finalize();
            return 0;
        }
    }
    else{
        printf("Please run the program with the script directory as an argument.\n");
        return -1;
    }

}