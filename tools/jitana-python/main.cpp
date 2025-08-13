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
    int call_count = 1;
};

// Define a directed graph using Boost
using Graph = boost::adjacency_list<boost::vecS, boost::vecS, boost::directedS, VertexProperties, EdgeProperties>;

// Map function names to vertex descriptors
using Vertex = boost::graph_traits<Graph>::vertex_descriptor;
using VertexMap = std::unordered_map<std::string, Vertex>;

struct Instruction {
    int offset;
    std::string opname;
    int opcode;
    int arg;
    std::string argval;
    std::string source_label; 
};

// Properties for a Dependency Graph Node
struct DependencyNode {
    std::string name;
    std::string type; // "File" or "Library"
    std::string color; // For Graphviz visualization
};

// Properties for a Control Flow Graph (CFG)
struct BasicBlock {
    int id = -1;
    int start_offset = -1;
    std::vector<Instruction> instructions;
};

struct ControlFlowEdge {
    std::string label; // e.g., "True", "False", "Unconditional", "Sequential"
};

// Edges don't need properties for this graph
using DependencyGraph = boost::adjacency_list<boost::vecS, boost::vecS, boost::directedS, DependencyNode>;
using DependencyVertex = boost::graph_traits<DependencyGraph>::vertex_descriptor;

using ControlFlowGraph = boost::adjacency_list<boost::vecS, boost::vecS, boost::directedS, BasicBlock, ControlFlowEdge>;
using CFGVertex = boost::graph_traits<ControlFlowGraph>::vertex_descriptor;

// Helper function to generate a CFG from a list of instructions
bool is_jump_instruction(const std::string& opname) {
    return opname.find("JUMP") != std::string::npos;
}

// Load Python module with a given name
PyObject* load_python_module(const char* module_name){
    PyObject *pName = PyUnicode_FromString(module_name);
    PyObject *pModule = PyImport_Import(pName);
    Py_DECREF(pName);
    if (!pModule) {
        PyErr_Print();
        fprintf(stderr, "Failed to load module %s\n", module_name);
    }
    return pModule;
}

// Load a Python function from a module
PyObject* load_python_function(PyObject *pModule, const char* function_name_str) {
    PyObject *pFunc = PyObject_GetAttrString(pModule, function_name_str);
    if (!pFunc || !PyCallable_Check(pFunc)) {
        if (PyErr_Occurred()) PyErr_Print();
        fprintf(stderr, "Function %s not found or not callable\n", function_name_str);
        Py_XDECREF(pFunc);
        return NULL;
    }
    return pFunc;
}

PyObject* load_class_function(PyObject* pModule, const char* class_name_str, const char* method_name_str) {
    PyObject* pClass = PyObject_GetAttrString(pModule, class_name_str);
    if (!pClass || !PyType_Check(pClass)) {
        if (PyErr_Occurred()) PyErr_Print();
        std::cerr << "Class " << class_name_str << " not found or not a class.\n";
        Py_XDECREF(pClass);
        return nullptr;
    }

    PyObject* pFunc = PyObject_GetAttrString(pClass, method_name_str); // This gets the method
    Py_DECREF(pClass);

    if (!pFunc || !PyCallable_Check(pFunc)) {
        if (PyErr_Occurred()) PyErr_Print();
        std::cerr << "Method " << method_name_str << " from class " << class_name_str << " not found or not callable.\n";
        Py_XDECREF(pFunc);
        return nullptr;
    }
    return pFunc;
}

void find_classes_and_functions(PyObject* pModule, std::vector<std::string>& classes, std::vector<std::string>& functions) {
    PyObject* attrs = PyObject_Dir(pModule);
    if (!attrs || !PyList_Check(attrs)) { // PyObject_Dir returns a list
        Py_XDECREF(attrs);
        std::cerr << "Could not get attributes of module." << std::endl;
        if(PyErr_Occurred()) PyErr_Print();
        return;
    }
    Py_ssize_t len = PyList_Size(attrs);

    for (Py_ssize_t i = 0; i < len; ++i) {
        PyObject* attrNameUnicode = PyList_GetItem(attrs, i);  // borrowed reference
        if (!attrNameUnicode) continue;
        const char* attrStr = PyUnicode_AsUTF8(attrNameUnicode);
        if (!attrStr) continue;


        PyObject* attr = PyObject_GetAttrString(pModule, attrStr); // Get attribute by string directly

        if (attr) {
            if (PyType_Check(attr)) { // Checks if it's a class
                classes.push_back(attrStr);
            } else if (PyFunction_Check(attr)) { // Checks if it's a Python function
                functions.push_back(attrStr);
            }
            Py_DECREF(attr);
        } else {
            PyErr_Clear(); // Clear error if attribute couldn't be fetched for some reason
        }
    }
    Py_DECREF(attrs);
}

void list_class_methods(PyObject* pClass, std::vector<std::string>& methods) {
    PyObject* dirList = PyObject_Dir(pClass); // Get attributes of the class
     if (!dirList || !PyList_Check(dirList)) {
        Py_XDECREF(dirList);
        std::cerr << "Could not get attributes of class." << std::endl;
        if(PyErr_Occurred()) PyErr_Print();
        return;
    }
    Py_ssize_t len = PyList_Size(dirList);

    for (Py_ssize_t i = 0; i < len; ++i) {
        PyObject* attrNameUnicode = PyList_GetItem(dirList, i);
        if(!attrNameUnicode) continue;
        const char* attrStr = PyUnicode_AsUTF8(attrNameUnicode);
        if (!attrStr) continue;

        PyObject* attr = PyObject_GetAttrString(pClass, attrStr);

        if (attr) {
            // Check if it's a function (method in class context) or a bound method
            // For classes, methods are typically PyFunction_Type before binding
            if (PyFunction_Check(attr) || PyMethod_Check(attr) || PyCallable_Check(attr)) {
                 // Further check: ensure it's not a class itself (nested class)
                if (!PyType_Check(attr)) { // if not a type object
                    methods.push_back(attrStr);
                }
            }
            Py_DECREF(attr);
        } else {
            PyErr_Clear();
        }
    }
    Py_DECREF(dirList);
}

// Print the bytecode of a function
void print_bytecode(PyObject *pFunc) {
    PyObject *pCode = PyObject_GetAttrString(pFunc, "__code__");
    if (!pCode) { // Might be a bound method, try __func__.__code__
        PyErr_Clear();
        PyObject* pFuncAttr = PyObject_GetAttrString(pFunc, "__func__");
        if (pFuncAttr) {
            pCode = PyObject_GetAttrString(pFuncAttr, "__code__");
            Py_DECREF(pFuncAttr);
        }
    }

    if (pCode) {
        PyObject *pBytecode = PyObject_GetAttrString(pCode, "co_code");
        if (pBytecode && PyBytes_Check(pBytecode)) {
            char *bytecode_data = PyBytes_AsString(pBytecode);
            Py_ssize_t bytecode_len = PyBytes_Size(pBytecode);
            
            // Ensure output directory exists
            std::filesystem::path output_dir("output");
            if (!std::filesystem::exists(output_dir)) {
                std::filesystem::create_directories(output_dir);
            }
            std::ofstream file("output/bytecode.txt");

            if (file.is_open()){
                file << "Bytecode for function: \n";
                for (Py_ssize_t k = 0; k < bytecode_len; k++) {
                    file << std::hex << std::setw(2) << std::setfill('0')
                         << static_cast<unsigned int>(bytecode_data[k] & 0xFF) << " ";
                }
                file << std::dec << "\n"; // Switch back to decimal for other outputs
                file.close();
            }
            else{
                std::cerr << "Error: Unable to create file output/bytecode.txt. \n";
            }
            Py_DECREF(pBytecode);
        } else {
            if(PyErr_Occurred()) PyErr_Print();
            std::cerr << "Could not get co_code or it's not bytes." << std::endl;
        }
        Py_DECREF(pCode);
    } else {
        if(PyErr_Occurred()) PyErr_Print();
        std::cerr << "Could not get __code__ object from function." << std::endl;
    }
}


std::vector<Instruction> disassemble_function(PyObject *pFunc) {
    std::vector<Instruction> instructions_vec; 

    PyObject *pCode = PyObject_GetAttrString(pFunc, "__code__");
    if (!pCode) {
        PyErr_Clear();
        PyObject *pFuncAttr = PyObject_GetAttrString(pFunc, "__func__");
        if (pFuncAttr) {
            pCode = PyObject_GetAttrString(pFuncAttr, "__code__");
            Py_DECREF(pFuncAttr);
        }
    }

    if (!pCode) {
        // PyErr_Print(); // Potentially noisy if it's a C function
        std::cerr << "Cannot disassemble: no __code__ object found (likely a built-in or C extension function for ";
        PyObject* repr = PyObject_Repr(pFunc);
        if (repr) {
            std::cerr << PyUnicode_AsUTF8(repr);
            Py_DECREF(repr);
        }
        std::cerr << ").\n";
        PyErr_Clear();
        return instructions_vec;
    }

    PyObject *dis_module = PyImport_ImportModule("dis");
    if (!dis_module) {
        std::cerr << "Failed to import 'dis' module\n";
        Py_DECREF(pCode);
        PyErr_Print();
        return instructions_vec;
    }

    PyObject *bytecode_class = PyObject_GetAttrString(dis_module, "Bytecode");
    if (!bytecode_class || !PyCallable_Check(bytecode_class)) {
        std::cerr << "Failed to access or call dis.Bytecode\n";
        Py_XDECREF(bytecode_class);
        Py_DECREF(dis_module);
        Py_DECREF(pCode);
        PyErr_Print();
        return instructions_vec;
    }

    PyObject *args = PyTuple_Pack(1, pCode);
    PyObject *bytecode_obj = PyObject_CallObject(bytecode_class, args);
    Py_DECREF(args);
    Py_DECREF(bytecode_class);
    Py_DECREF(dis_module);
    Py_DECREF(pCode); // Decref pCode once it's used to create bytecode_obj

    if (!bytecode_obj) {
        std::cerr << "Failed to create Bytecode object\n";
        PyErr_Print();
        return instructions_vec;
    }

    PyObject *iterator = PyObject_GetIter(bytecode_obj);
    Py_DECREF(bytecode_obj);

    if (!iterator) {
        std::cerr << "Failed to get iterator from Bytecode object\n";
        PyErr_Print();
        return instructions_vec;
    }

    PyObject* instr_obj; 
    while ((instr_obj = PyIter_Next(iterator))) {
        Instruction current_inst{}; 

        PyObject* opname_obj = PyObject_GetAttrString(instr_obj, "opname");
        if (opname_obj && PyUnicode_Check(opname_obj)) {
            current_inst.opname = PyUnicode_AsUTF8(opname_obj);
            Py_DECREF(opname_obj);
        }

        PyObject* offset_obj = PyObject_GetAttrString(instr_obj, "offset");
        if (offset_obj && PyLong_Check(offset_obj)) {
            current_inst.offset = PyLong_AsLong(offset_obj);
        } else {
            current_inst.offset = -1; // Should not happen for valid instructions
        }
        Py_XDECREF(offset_obj);

        PyObject* opcode_obj = PyObject_GetAttrString(instr_obj, "opcode");
        if (opcode_obj && PyLong_Check(opcode_obj)) {
            current_inst.opcode = PyLong_AsLong(opcode_obj);
            Py_DECREF(opcode_obj);
        }

        PyObject* arg_obj = PyObject_GetAttrString(instr_obj, "arg");
        if (arg_obj && PyLong_Check(arg_obj)) {
            current_inst.arg = PyLong_AsLong(arg_obj);
        } else {
            current_inst.arg = -1; 
        }
        Py_XDECREF(arg_obj);


        PyObject* argval_obj = PyObject_GetAttrString(instr_obj, "argval");
        if (argval_obj) {
            if (PyUnicode_Check(argval_obj)) {
                current_inst.argval = PyUnicode_AsUTF8(argval_obj);
            } else if (PyLong_Check(argval_obj)) {
                current_inst.argval = std::to_string(PyLong_AsLong(argval_obj));
            } else if (argval_obj == Py_None) {
                current_inst.argval = "None";
            } else {
                PyObject* repr = PyObject_Repr(argval_obj);
                if (repr && PyUnicode_Check(repr)) {
                    current_inst.argval = PyUnicode_AsUTF8(repr);
                    Py_DECREF(repr);
                } else {
                    current_inst.argval = "<unknown_argval_type>";
                    if(PyErr_Occurred()) PyErr_Clear();
                }
            }
            Py_DECREF(argval_obj);
        } else {
             PyErr_Clear(); // If argval is not present (e.g. for opcodes without it)
            current_inst.argval = ""; 
        }
        instructions_vec.push_back(current_inst);
        Py_DECREF(instr_obj); 
    }
    Py_DECREF(iterator);
    if (PyErr_Occurred()) { // Catch errors during iteration
        PyErr_Print();
        std::cerr << "Error occurred during bytecode iteration." << std::endl;
    }
    return instructions_vec;
}


std::pair<int, int> get_instruction_stack_effect(const Instruction& inst) {
    const std::string& opname = inst.opname;
    int oparg = inst.arg;

    // Use a static map for common opcodes with fixed stack effects
    static const std::map<std::string, std::pair<int, int>> fixed_effects = {
        // General Load operations
        {"LOAD_CONST", {0, 1}},
        {"LOAD_NAME", {0, 1}},
        {"LOAD_GLOBAL", {0, 1}},
        {"LOAD_FAST", {0, 1}},
        {"LOAD_DEREF", {0, 1}},
        {"LOAD_CLOSURE", {0, 1}},
        {"LOAD_ATTR", {1, 1}},
        {"LOAD_METHOD", {1, 1}},
        {"LOAD_FAST_LOAD_FAST", {0, 2}},
        {"LOAD_SUPER_ATTR", {3, 1}},
        {"COPY", {0, 1}}, // Copies item at TOS - oparg to TOS. Net +1.
        {"SWAP", {0, 0}}, // Swaps TOS and TOS - oparg. Net 0.

        // Store operations
        {"STORE_FAST", {1, 0}},
        {"STORE_GLOBAL", {1, 0}},
        {"STORE_NAME", {1, 0}},
        {"STORE_DEREF", {1, 0}},
        {"DELETE_FAST", {1, 0}},
        {"DELETE_GLOBAL", {1, 0}},
        {"DELETE_NAME", {1, 0}},
        {"DELETE_DEREF", {1, 0}},
        {"STORE_ATTR", {2, 0}},
        {"DELETE_ATTR", {2, 0}},

        // Stack manipulation
        {"POP_TOP", {1, 0}},
        {"DUP_TOP", {1, 2}},
        {"DUP_TOP_TWO", {2, 4}},
        {"ROT_TWO", {2, 2}},
        {"ROT_THREE", {3, 3}},
        {"ROT_FOUR", {4, 4}},

        // Unary operations
        {"UNARY_POSITIVE", {1, 1}},
        {"UNARY_NEGATIVE", {1, 1}},
        {"UNARY_NOT", {1, 1}},
        {"UNARY_INVERT", {1, 1}},

        // Other common ones
        {"RETURN_VALUE", {1, 0}},
        {"YIELD_VALUE", {1, 0}},
        {"CONTAINS_OP", {2, 1}},
        {"IS_OP", {2, 1}},
        {"COMPARE_OP", {2, 1}},
        
        

        // Jumps (conditional pops)
        {"POP_JUMP_IF_FALSE", {1, 0}},
        {"POP_JUMP_IF_TRUE", {1, 0}},
        {"POP_JUMP_FORWARD_IF_FALSE", {1, 0}},
        {"POP_JUMP_FORWARD_IF_TRUE", {1, 0}},
        {"POP_JUMP_BACKWARD_IF_FALSE", {1, 0}},
        {"POP_JUMP_BACKWARD_IF_TRUE", {1, 0}},
        {"JUMP_IF_FALSE_OR_POP", {1, 0}}, // Specific to some Python versions
        {"JUMP_IF_TRUE_OR_POP", {1, 0}},   // Specific to some Python versions

        // No stack effect
        {"NOP", {0, 0}},
        {"RESUME", {0, 0}},
        {"PRECALL", {0, 0}},
        {"KW_NAMES", {0, 0}},
        {"COPY_FREE_VARS", {0, 0}},
        {"MAKE_CELL", {0, 0}},
        {"PUSH_NULL", {0, 1}}, // Used with CALL for methods in Py 3.11+
        {"GET_ITER", {1, 1}},
        {"GET_YIELD_FROM_ITER", {1, 1}},
        {"FOR_ITER", {1, 2}}, // iterator -> iterator, next_item (or jumps)

        // Operations that pop 2, push 1 (often for in-place modifications or combined ops)
        {"LIST_EXTEND", {2, 1}}, // Adjusted based on previous discussion
        {"SET_ADD", {2, 1}},
        {"DICT_UPDATE", {2, 1}}
    };

    // Try to find the opcode in the map
    auto it = fixed_effects.find(opname);
    if (it != fixed_effects.end()) {
        return it->second;
    }

    // Handle opcodes with stack effects dependent on `oparg` or patterns
    if (opname == "STORE_SUBSCR") {
        return {3, 0};
    } else if (opname == "DELETE_SUBSCR") {
        return {2, 0};
    } else if (opname.rfind("BINARY_", 0) == 0 || opname.rfind("INPLACE_", 0) == 0) {
        // BINARY_OP (Py 3.11+) and older BINARY_ADD etc.
        return {2, 1};
    } else if (opname == "BUILD_TUPLE" || opname == "BUILD_LIST" || opname == "BUILD_SET") {
        return {oparg, 1};
    } else if (opname == "BUILD_MAP") {
        return {oparg * 2, 1};
    } else if (opname == "BUILD_CONST_KEY_MAP") {
        return {oparg + 1, 1};
    } else if (opname == "BUILD_STRING") {
        return {oparg, 1};
    } else if (opname == "BUILD_SLICE") {
        return {(oparg == 3) ? 3 : 2, 1};
    } else if (opname == "FORMAT_VALUE") {
        return {((oparg & 0x04) == 0x04) ? 2 : 1, 1};
    } else if (opname == "UNPACK_SEQUENCE" || opname == "UNPACK_EX") {
        return {1, oparg};
    }
    // Corrected Call operations stack effects
    else if (opname == "CALL_FUNCTION" || opname == "CALL") {
        // Pops N arguments + 1 callable, pushes 1 return value
        return {inst.arg + 1, 1};
    } else if (opname == "CALL_METHOD") {
        // Pops N arguments + 1 callable + 1 (for PUSH_NULL/self), pushes 1 return value
        // Note: CALL_METHOD (Py 3.8-3.10) pops N args + 1 method object.
        // For Py 3.11+, LOAD_METHOD + PUSH_NULL + CALL is common.
        // If it's a real CALL_METHOD, it's 1 (method) + N (args).
        return {inst.arg + 1, 1};
    } else if (opname == "CALL_KW" || opname == "CALL_FUNCTION_KW") {
        // Pops N total args + 1 kwnames_tuple + 1 callable, pushes 1 return value
        return {inst.arg + 1 + 1, 1}; // inst.arg is N args, +1 for kwnames, +1 for callable
    } else if (opname == "CALL_FUNCTION_EX") {
        // Pops 1 args tuple (+1 kwargs dict if arg & 0x01) + 1 callable, pushes 1 return value
        int pops = 1 + 1; // 1 args tuple + 1 callable
        if (inst.arg & 0x01) { // HAS_KEYWORDS flag
            pops += 1; // +1 kwargs dict
        }
        return {pops, 1};
    }


    // Default case for unknown or unhandled opcodes
    // You might want to log this or throw an error for unhandled cases
    // std::cout << "Debug: Stack effect for opcode '" << opname << "' (opcode " << inst.opcode << ") not explicitly defined. Assuming pop=0, push=0." << std::endl;
    return {0, 0};
}

// Recursive helper:
// - instructions: The list of bytecode instructions.
// - currentIndex: The index of the instruction we are currently examining (traces backwards).
// - itemsToAccountFor: How many items on the stack (from the perspective *before* currentIndex executed)
//                      are "covering" the item we are trying to find.
//                      When this is 0, it means instructions[currentIndex] is the one that pushed our target item.
// - nameParts: Stores the parts of the callable's name in reverse order (e.g., ["method", "attr", "base_obj"]).
// - maxDepth: Safety for recursion.
// Returns the index of the *base* load instruction (LOAD_GLOBAL/FAST/DEREF) or -1.
int find_loader_recursive_stack_trace(
    const std::vector<Instruction>& instructions,
    int currentIndex,
    int itemsToAccountFor,
    std::vector<std::string>& nameParts,
    int currentDepth = 0,
    const int maxDepth = 20) // Increased maxDepth slightly
{
    if (currentIndex < 0 || currentDepth > maxDepth) {
        return -1; // Reached beginning or max recursion depth
    }

    const Instruction& currentInst = instructions[currentIndex];
    std::pair<int, int> effect = get_instruction_stack_effect(currentInst);
    int itemsPoppedByCurrent = effect.first;
    int itemsPushedByCurrent = effect.second;

    // If itemsToAccountFor is less than what currentInst pushed,
    // it means currentInst is responsible for pushing our target item.
    // The specific item is itemsPushedByCurrent - 1 - itemsToAccountFor (0-indexed from bottom of pushes).
    // For most loaders (LOAD_GLOBAL, LOAD_ATTR etc.), itemsPushedByCurrent is 1.
    // So if itemsToAccountFor == 0, currentInst pushed the item we want.
    if (itemsToAccountFor < itemsPushedByCurrent) {
        if (currentInst.opname == "LOAD_GLOBAL" || currentInst.opname == "LOAD_FAST" ||
            currentInst.opname == "LOAD_DEREF") {
            nameParts.push_back(currentInst.argval);
            return currentIndex; // Found the base of the chain
        } else if (currentInst.opname == "LOAD_ATTR" || currentInst.opname == "LOAD_METHOD") {
            nameParts.push_back(currentInst.argval);
            // This instruction popped 1 item (the object) and pushed 1 item (the attribute/method).
            // We've accounted for the push. Now we need to find what pushed the item that was popped.
            // The item popped was at the same "logical depth" before this instruction ran.
            // So, the `itemsToAccountFor` for the *previous* instruction (relative to the object)
            // should be the same as the current `itemsToAccountFor` that led us here (which was 0 for this path).
            return find_loader_recursive_stack_trace(instructions, currentIndex - 1, itemsToAccountFor, nameParts, currentDepth + 1, maxDepth);
        } else if (currentInst.opname == "LOAD_CONST") {
            nameParts.push_back("<const:" + currentInst.argval + ">"); // Represent constants
            return currentIndex;
        } else if (currentInst.opname == "PUSH_NULL" && itemsToAccountFor == 0) {
            nameParts.push_back("<null_for_method_call>"); // Placeholder for PUSH_NULL in call sequences
            return currentIndex; // Treat as a "loader" for null
        }
        // If it's another type of instruction that pushed our target (e.g., MAKE_FUNCTION),
        // we might not be able to name it further here. For now, stop.
        // std::cout << "Debug: Landed on non-loader that pushed target: " << currentInst.opname << std::endl;
        return -1; // Or return currentIndex if this instr is considered the "source"
    }

    // If currentInst did not push our target, adjust itemsToAccountFor based on its net stack effect.
    // We are going backwards, so if currentInst had a net effect of +X (pushed X more than popped),
    // then for the preceding state, we need to account for X *fewer* items.
    int netStackChangeByCurrent = itemsPushedByCurrent - itemsPoppedByCurrent;
    return find_loader_recursive_stack_trace(instructions, currentIndex - 1, itemsToAccountFor - netStackChangeByCurrent, nameParts, currentDepth + 1, maxDepth);
}


int find_callable_info_stack_based(
    const std::vector<Instruction>& instructions,
    int call_instruction_idx,
    std::string& base_name_out,
    std::string& attr_name_out,
    std::string& full_name_out)
{
    base_name_out.clear();
    attr_name_out.clear();
    full_name_out.clear();

    if (call_instruction_idx <= 0 || static_cast<size_t>(call_instruction_idx) >= instructions.size()) {
        return -1;
    }

    const Instruction& call_instr = instructions[call_instruction_idx];
    int num_stack_items_for_call_above_callable = 0; // Number of args, kwnames etc. *above* the callable

    // Determine stack consumption based on Python's calling conventions
    // These counts are items *consumed by the CALL* that were on top of the callable.
    // The callable itself is one more item.
    if (call_instr.opname == "CALL_FUNCTION" || call_instr.opname == "CALL") { // CALL for Py3.11+
        num_stack_items_for_call_above_callable = call_instr.arg; // N arguments
    } else if (call_instr.opname == "CALL_METHOD") { // oparg = N arguments
        num_stack_items_for_call_above_callable = call_instr.arg; // N arguments (method object itself includes 'self' or NULL)
    } else if (call_instr.opname == "CALL_KW" || call_instr.opname == "CALL_FUNCTION_KW") { // oparg = N total args (pos + kw)
        num_stack_items_for_call_above_callable = call_instr.arg + 1; // N arguments + 1 kwnames_tuple
    } else if (call_instr.opname == "CALL_FUNCTION_EX") {
        num_stack_items_for_call_above_callable = 1; // The `args` tuple
        if (call_instr.arg & 0x01) { // HAS_KEYWORDS flag means a `kwargs` dict is also present
            num_stack_items_for_call_above_callable += 1;
        }
    } else {
        // std::cerr << "StackFind: Unhandled CALL type: " << call_instr.opname << " at index " << call_instruction_idx << std::endl;
        return -1; // Not a recognized call instruction
    }

    // `num_stack_items_for_call_above_callable` is the number of items popped by the CALL op
    // *after* the callable itself was popped. So, if this value is N, the callable was at TOS-N
    // (0-indexed from TOS) right before the CALL op started popping.
    // We are looking for the instruction that pushed the item that will end up at this relative depth.
    // So, the initial `itemsToAccountFor` is this N.

    std::vector<std::string> name_parts_reversed;
    int base_loader_idx = find_loader_recursive_stack_trace(
        instructions,
        call_instruction_idx - 1, // Start searching from instruction before the call
        num_stack_items_for_call_above_callable,
        name_parts_reversed);

    if (base_loader_idx != -1 && !name_parts_reversed.empty()) {
        std::reverse(name_parts_reversed.begin(), name_parts_reversed.end()); // Names were collected base->attr->method

        base_name_out = name_parts_reversed[0];
        full_name_out = base_name_out;

        for (size_t i = 1; i < name_parts_reversed.size(); ++i) {
            full_name_out += "." + name_parts_reversed[i];
            if (i == name_parts_reversed.size() - 1) { // The last part is the final attribute/method name
                attr_name_out = name_parts_reversed[i];
            }
        }
        return base_loader_idx;
    }

    // std::cout << "StackFind: Failed to find callable for " << call_instr.opname << " at " << call_instruction_idx << std::endl;
    return -1;
}



void disassemble_called_functions_recursive(
    PyObject* context_function, // The Python function object whose bytecode (instructions) we are analyzing
    const std::vector<Instruction>& instructions, // Bytecode of context_function
    std::map<std::string, std::vector<Instruction>>& called_functions, // Accumulator for all disassembled functions
    std::set<std::string>& visited // To avoid re-processing and infinite recursion
) {
    // std::cout << "Disassembling context: ";
    // PyObject* context_repr = PyObject_Repr(context_function);
    // if(context_repr) { std::cout << PyUnicode_AsUTF8(context_repr) << std::endl; Py_DECREF(context_repr); }
    // else { std::cout << "(unknown)" << std::endl; PyErr_Clear(); }


    PyObject* globals_dict = nullptr;
    if (PyFunction_Check(context_function)) { // Regular functions have __globals__
        globals_dict = PyFunction_GetGlobals(context_function); // Borrowed reference
    } else if (PyMethod_Check(context_function)) { // Bound methods have __func__ which has __globals__
        PyObject* func_attr = PyObject_GetAttrString(context_function, "__func__");
        if (func_attr && PyFunction_Check(func_attr)) {
            globals_dict = PyFunction_GetGlobals(func_attr); // Borrowed reference
        }
        Py_XDECREF(func_attr);
    }

    if (!globals_dict) {
        std::cerr << "Could not get globals dictionary from function/method.\n";
        // PyErr_Print(); // Can be noisy
        return;
    }
    // No Py_INCREF on globals_dict as it's borrowed from PyFunction_GetGlobals or context.

    for (size_t i = 0; i < instructions.size(); ++i) {
        const Instruction& current_instruction = instructions[i];
        // Check for various call opcodes
        if (current_instruction.opname.rfind("CALL", 0) == 0) { // Catches CALL, CALL_FUNCTION, CALL_METHOD, CALL_KW etc.
            std::string base_name;
            std::string attr_name;
            std::string called_func_identifier; // This will be like "mod.func" or "self.method" or "glob_func"

            int load_instruction_idx = find_callable_info_stack_based(instructions, i, base_name, attr_name, called_func_identifier);

            if (load_instruction_idx >= 0 && !called_func_identifier.empty() && visited.find(called_func_identifier) == visited.end()) {
                PyObject* target_callable = nullptr;

                if (base_name == "self") {
                    if (PyMethod_Check(context_function) || PyObject_HasAttrString(context_function, "__self__")) {
                        PyObject* pSelf = PyObject_GetAttrString(context_function, "__self__"); // instance object
                        if (pSelf) {
                            if (!attr_name.empty()) {
                                // PyObject_GetAttrString on the instance (pSelf) handles MRO for inherited methods.
                                target_callable = PyObject_GetAttrString(pSelf, attr_name.c_str());
                                if (!target_callable) {
                                    PyErr_Clear(); // Clear error if attr not found, e.g. dynamic attribute
                                     std::cout << "Debug: Attribute '" << attr_name << "' not found on 'self' instance for call '" << called_func_identifier << "'." << std::endl;
                                }
                            } else {
                                std::cerr << "Warning: 'self' used as callable without attribute: " << called_func_identifier << std::endl;
                            }
                            Py_DECREF(pSelf);
                        } else {
                             PyErr_Clear();
                             std::cerr << "Warning: Could not get __self__ from context_function while processing 'self." << attr_name << "'." << std::endl;
                        }
                    } else {
                        std::cerr << "Warning: '" << base_name << "' (likely self) encountered, but context_function is not a recognized method or has no __self__." << std::endl;
                    }
                } else { // Not 'self'. Could be global module, global function, or other.
                    PyObject* loaded_item_from_globals = PyDict_GetItemString(globals_dict, base_name.c_str()); // BORROWED reference

                    if (loaded_item_from_globals) {
                        Py_INCREF(loaded_item_from_globals); // Now we own a reference
                        if (!attr_name.empty()) { // e.g. os.path.join (base_name="os", attr_name="join", full_name="os.path.join")
                                                  // The find_callable_info simplifies attr_name to the last component.
                                                  // We need to resolve the full attribute chain from loaded_item_from_globals.
                            
                            // Reconstruct full attribute access from base_name if full_name_out has multiple dots
                            std::string full_attr_path = called_func_identifier;
                            if (full_attr_path.rfind(base_name + ".", 0) == 0) {
                                full_attr_path = full_attr_path.substr(base_name.length() + 1);
                            }


                            PyObject* current_obj = loaded_item_from_globals;
                            Py_INCREF(current_obj); // Start with a ref to current_obj

                            std::string segment;
                            std::stringstream ss(full_attr_path);
                            bool first_segment = true;
                            PyObject* temp_obj_holder = nullptr;

                            while(std::getline(ss, segment, '.')) {
                                if (first_segment && segment == base_name) { // Skip if first part of path is base_name itself
                                     first_segment = false; continue;
                                }
                                if (segment == attr_name && ss.peek() == EOF) { // Last segment is the attr_name
                                     temp_obj_holder = PyObject_GetAttrString(current_obj, segment.c_str());
                                     Py_DECREF(current_obj); // Current_obj becomes the new temp_obj_holder or is replaced
                                     current_obj = temp_obj_holder;
                                     break; 
                                } else { // Intermediate attribute
                                     temp_obj_holder = PyObject_GetAttrString(current_obj, segment.c_str());
                                     Py_DECREF(current_obj);
                                     current_obj = temp_obj_holder;
                                     if (!current_obj) break; // Path broken
                                }
                                first_segment = false;
                            }
                            target_callable = current_obj; // This is the final object/method

                        } else { // Direct call to a global name
                            target_callable = loaded_item_from_globals; // Already incref'd
                            Py_INCREF(target_callable); // Target callable needs its own ref
                        }
                        Py_DECREF(loaded_item_from_globals); // Decref our initial incref
                    } else {
                        // If not in globals, it might be a local variable from LOAD_FAST that's an object.
                        // This is harder to resolve statically without frame information.
                        std::cout << "Debug: Base name '" << base_name << "' not 'self' and not in globals for call '" << called_func_identifier << "'. Resolution might fail." << std::endl;
                    }
                }

                if (target_callable) {
                    if (PyCallable_Check(target_callable)) {
                        if (PyFunction_Check(target_callable) || PyMethod_Check(target_callable)) {
                            // std::cout << "Disassembling successfully resolved call to: " << called_func_identifier << std::endl;
                            std::vector<Instruction> inner_instructions_vec = disassemble_function(target_callable);
                            if (!inner_instructions_vec.empty()) {
                                called_functions[called_func_identifier] = inner_instructions_vec;
                                visited.insert(called_func_identifier);
                                disassemble_called_functions_recursive(target_callable, inner_instructions_vec, called_functions, visited);
                            }
                        } else if (PyCFunction_Check(target_callable)) {
                            // std::cout << "Skipping disassembly of C function: " << called_func_identifier << std::endl;
                            if (called_functions.find(called_func_identifier) == called_functions.end()) {
                                called_functions[called_func_identifier] = {}; // Empty instructions for C functions
                                visited.insert(called_func_identifier);
                            }
                        } else {
                            std::cout << "Object " << called_func_identifier << " is callable but not a standard Python function/method or C function. Adding to graph as unresolved." << std::endl;
                            // Add to called_functions map with empty instructions to ensure it's included in graph processing
                            if (called_functions.find(called_func_identifier) == called_functions.end()) {
                                called_functions[called_func_identifier] = {}; // Mark as unresolved/non-disassemblable
                            }
                            visited.insert(called_func_identifier); // Mark as visited to prevent reprocessing
                        }
                    } else {
                        // std::cout << "Resolved target " << called_func_identifier << " is not callable." << std::endl;
                    }
                    Py_DECREF(target_callable);
                } else {
                     std::cout << "Could not resolve PyObject for call target: '" << called_func_identifier << "' (base: '" << base_name << "', attr: '" << attr_name << "')." << std::endl;
                    if (!called_func_identifier.empty() && called_functions.find(called_func_identifier) == called_functions.end()) {
                        called_functions[called_func_identifier] = {}; // Mark as unresolved
                        visited.insert(called_func_identifier);
                        // std::cout << "Marked '" << called_func_identifier << "' as an unresolved call target." << std::endl;
                    }
                     if(PyErr_Occurred()) PyErr_Clear(); // Clear any errors from failed lookups
                }
            } else if (load_instruction_idx < 0 && !current_instruction.argval.empty() && instructions[i].opname.rfind("CALL",0)==0) {
                // Fallback for simple calls where find_callable_info might fail but argval has a name
                // This is a rough heuristic, often applies to CALL_FUNCTION where argval is the function name directly (less common)
                // or if the called item is complexly loaded.
                std::string potential_direct_call_name = current_instruction.argval; // If argval directly names the function
                if(!potential_direct_call_name.empty() && visited.find(potential_direct_call_name) == visited.end()){
                     std::cout << "Attempting fallback resolution for call argval: " << potential_direct_call_name << std::endl;
                     // Try to load it from globals directly
                     PyObject* fallback_target = PyDict_GetItemString(globals_dict, potential_direct_call_name.c_str()); // Borrowed
                     if (fallback_target && PyCallable_Check(fallback_target)) {
                        Py_INCREF(fallback_target);
                        if (PyFunction_Check(fallback_target) || PyMethod_Check(fallback_target)) {
                            std::vector<Instruction> inner_fallback = disassemble_function(fallback_target);
                            if (!inner_fallback.empty()) {
                                called_functions[potential_direct_call_name] = inner_fallback;
                                visited.insert(potential_direct_call_name);
                                disassemble_called_functions_recursive(fallback_target, inner_fallback, called_functions, visited);
                            }
                        } else if (PyCFunction_Check(fallback_target)) {
                             if (called_functions.find(potential_direct_call_name) == called_functions.end()) {
                                called_functions[potential_direct_call_name] = {};
                                visited.insert(potential_direct_call_name);
                            }
                        } else {
                            std::cout << "Fallback target " << potential_direct_call_name << " is callable but not standard func/method/C. Adding to graph." << std::endl;
                            if (called_functions.find(potential_direct_call_name) == called_functions.end()) {
                                called_functions[potential_direct_call_name] = {}; // Mark as unresolved
                            }
                            visited.insert(potential_direct_call_name);
                        }
                        Py_DECREF(fallback_target);
                     } else if (!fallback_target && called_functions.find(potential_direct_call_name) == called_functions.end()) {
                        called_functions[potential_direct_call_name] = {}; // Mark as unresolved
                        visited.insert(potential_direct_call_name);
                     }
                }
            }
        }
    }
}


std::map<std::string, std::vector<Instruction>> disassemble_all_called_functions(PyObject* original_function, const char* initial_func_name) {
    std::map<std::string, std::vector<Instruction>> all_called_functions;
    std::set<std::string> visited_functions;

    std::vector<Instruction> top_level_instructions = disassemble_function(original_function);
    if (!top_level_instructions.empty()) {
        all_called_functions[initial_func_name] = top_level_instructions;
        visited_functions.insert(initial_func_name);
        disassemble_called_functions_recursive(original_function, top_level_instructions, all_called_functions, visited_functions);
    }
    
    std::cout << "\n--- Disassembled Functions Summary ---" << std::endl;
    for (const auto& entry : all_called_functions) {
        std::cout << "Function: " << entry.first << " (" << entry.second.size() << " instructions)" << std::endl;
    }
    std::cout << "--- End Summary ---" << std::endl;
    return all_called_functions;
}

// Main function to generate a CFG from a list of instructions
ControlFlowGraph generate_cfg_from_instructions(const std::vector<Instruction>& instructions) {
    ControlFlowGraph cfg;
    if (instructions.empty()) {
        return cfg;
    }

    // Map from an instruction's byte offset to its index in our vector
    std::map<int, int> offset_to_index;
    for (size_t i = 0; i < instructions.size(); ++i) {
        offset_to_index[instructions[i].offset] = i;
    }

    // --- PASS 1: Find leader offsets ---
    // A leader is the first instruction of a basic block.
    std::set<int> leader_offsets;
    leader_offsets.insert(instructions.front().offset); // The first instruction is always a leader

    for (size_t i = 0; i < instructions.size(); ++i) {
        const auto& instr = instructions[i];
        if (is_jump_instruction(instr.opname)) {
            // The target of a jump is a leader
            int target_offset = std::stoi(instr.argval);
            if (offset_to_index.count(target_offset)) {
                leader_offsets.insert(target_offset);
            }
            
            // The instruction immediately following a jump is also a leader
            if (i + 1 < instructions.size()) {
                leader_offsets.insert(instructions[i + 1].offset);
            }
        }
        // Also consider RETURN_VALUE or other block-ending instructions
        else if (instr.opname == "RETURN_VALUE" || instr.opname == "RETURN_CONST") {
             if (i + 1 < instructions.size()) {
                leader_offsets.insert(instructions[i + 1].offset);
            }
        }
    }

    // --- PASS 2: Create basic blocks (nodes) ---
    if (leader_offsets.empty()) return cfg;

    std::map<int, CFGVertex> block_start_to_vertex;
    int block_id_counter = 0;
    for (int offset : leader_offsets) {
        CFGVertex v = boost::add_vertex(cfg);
        cfg[v].id = block_id_counter++;
        cfg[v].start_offset = offset;
        block_start_to_vertex[offset] = v;
    }

    // Populate blocks with their instructions
    for (const auto& instr : instructions) {
        auto it = leader_offsets.upper_bound(instr.offset);
        int leader_for_this_instr = *(--it);
        CFGVertex v = block_start_to_vertex[leader_for_this_instr];
        cfg[v].instructions.push_back(instr);
    }
    
    // --- PASS 3: Create Edges ---
    for (const auto& pair : block_start_to_vertex) {
        int offset = pair.first;
        CFGVertex u = pair.second;

        const Instruction& last_instr = cfg[u].instructions.back();

        if (is_jump_instruction(last_instr.opname)) {
            int target_offset = std::stoi(last_instr.argval);
            if (block_start_to_vertex.count(target_offset)) {
                CFGVertex v_target = block_start_to_vertex[target_offset];
                auto edge = boost::add_edge(u, v_target, cfg).first;
                // Distinguish conditional from unconditional jumps
                if (last_instr.opname.find("POP_JUMP") != std::string::npos) {
                     cfg[edge].label = std::string("Jump (if ") + (last_instr.opname.find("FALSE") != std::string::npos ? "False" : "True") + ")";
                } else {
                     cfg[edge].label = "Unconditional";
                }
            }
            
            // For conditional jumps, add the fall-through edge
            if (last_instr.opname.rfind("POP_JUMP", 0) == 0) {
                 auto next_leader_it = leader_offsets.find(offset);
                 if(next_leader_it != leader_offsets.end() && ++next_leader_it != leader_offsets.end()){
                     CFGVertex v_fallthrough = block_start_to_vertex[*next_leader_it];
                     auto edge = boost::add_edge(u, v_fallthrough, cfg).first;
                     cfg[edge].label = std::string("Fall-through (if ") + (last_instr.opname.find("FALSE") != std::string::npos ? "True" : "False") + ")";
                 }
            }

        } else if (last_instr.opname != "RETURN_VALUE" && last_instr.opname != "RETURN_CONST" && last_instr.opname.rfind("RAISE", 0) != 0) {
            // Not a jump or return, so it's a sequential flow to the next block
            auto next_leader_it = leader_offsets.find(offset);
            if (next_leader_it != leader_offsets.end() && ++next_leader_it != leader_offsets.end()) {
                CFGVertex v_next = block_start_to_vertex[*next_leader_it];
                auto edge = boost::add_edge(u, v_next, cfg).first;
                cfg[edge].label = "Sequential";
            }
        }
    }

    return cfg;
}

// New function to write the generated CFG to a .dot file
void write_cfg_to_dot(const std::string& filename, const ControlFlowGraph& cfg, const std::string& func_name) {
    std::ofstream dot_file(filename);
    if (!dot_file) {
        std::cerr << "Error: Unable to open " << filename << " for writing.\n";
        return;
    }

    // --- FIX IS HERE ---
    // Sanitize the function name to create a valid Graphviz identifier.
    std::string safe_graph_name = func_name;
    std::replace(safe_graph_name.begin(), safe_graph_name.end(), '.', '_');

    // Use the sanitized name for the graph and its label.
    dot_file << "digraph " << safe_graph_name << "_CFG {\n";
    dot_file << "    labelloc=\"t\";\n";
    dot_file << "    label=\"Control Flow Graph for " << func_name << "\";\n"; // Label can keep original name
    // --- END OF FIX ---

    dot_file << "    node [shape=box, fontname=\"Courier New\"];\n";

    auto vertices = boost::vertices(cfg);
    for (auto it = vertices.first; it != vertices.second; ++it) {
        const auto& block = cfg[*it];
        dot_file << "    Node" << block.id << " [label=\"";
        dot_file << "Block " << block.id << " (starts at " << block.start_offset << ")\\l\\l";
        for (const auto& instr : block.instructions) {
            dot_file << std::to_string(instr.offset) << ": " << instr.opname;
            if (instr.arg != -1) {
                // Escape quotes in argval to prevent breaking the dot file
                std::string safe_argval = instr.argval;
                size_t pos = 0;
                while ((pos = safe_argval.find('"', pos)) != std::string::npos) {
                    safe_argval.replace(pos, 1, "\\\"");
                    pos += 2;
                }
                dot_file << " " << safe_argval;
            }
            dot_file << "\\l";
        }
        dot_file << "\"];\n";
    }

    auto edges = boost::edges(cfg);
    for (auto it = edges.first; it != edges.second; ++it) {
        CFGVertex u = boost::source(*it, cfg);
        CFGVertex v = boost::target(*it, cfg);
        dot_file << "    Node" << cfg[u].id << " -> Node" << cfg[v].id;
        dot_file << " [label=\"" << cfg[*it].label << "\"];\n";
    }

    dot_file << "}\n";
    dot_file.close();
}

void generate_dependency_graph(
    const std::string& initial_script_name,
    const std::string& script_dir
) {
    DependencyGraph dep_graph;
    std::map<std::string, DependencyVertex> known_nodes;
    std::set<std::string> processed_files;
    std::queue<std::string> files_to_process;

    // 1. Start the queue with the initial script
    files_to_process.push(initial_script_name);

    // 2. Process files until the queue is empty
    while (!files_to_process.empty()) {
        std::string current_file_name = files_to_process.front();
        files_to_process.pop();

        if (processed_files.count(current_file_name)) {
            continue; // Already processed, skip to avoid cycles
        }
        processed_files.insert(current_file_name);

        // 3. Get or create the graph node for the current file
        DependencyVertex current_vertex;
        if (known_nodes.find(current_file_name) == known_nodes.end()) {
            current_vertex = boost::add_vertex(dep_graph);
            known_nodes[current_file_name] = current_vertex;
        } else {
            current_vertex = known_nodes[current_file_name];
        }
        dep_graph[current_vertex].name = current_file_name;
        dep_graph[current_vertex].type = "File";
        dep_graph[current_vertex].color = "skyblue";


        // 4. Analyze the current file to find its dependencies
        std::set<std::string> dependencies;
        PyObject* pModule = load_python_module(current_file_name.c_str());
        if (!pModule) {
            std::cerr << "Warning: Could not load module " << current_file_name << " to scan for dependencies." << std::endl;
            continue;
        }

        std::vector<std::string> functions, classes;
        find_classes_and_functions(pModule, classes, functions); // Find all functions in this module
        
        for (const auto& func_name : functions) {
            PyObject* pFunc = load_python_function(pModule, func_name.c_str());
            if (pFunc) {
                auto instructions = disassemble_function(pFunc);
                for (const auto& inst : instructions) {
                    if (inst.opname == "IMPORT_NAME") {
                        dependencies.insert(inst.argval);
                    }
                }
                Py_DECREF(pFunc);
            }
        }
        Py_DECREF(pModule);


        // 5. Process the found dependencies
        for (const auto& dep_name : dependencies) {
            DependencyVertex dep_vertex;
            if (known_nodes.find(dep_name) == known_nodes.end()) {
                dep_vertex = boost::add_vertex(dep_graph);
                known_nodes[dep_name] = dep_vertex;
            } else {
                dep_vertex = known_nodes[dep_name];
            }
            dep_graph[dep_vertex].name = dep_name;

            // Add an edge from the current file to its dependency
            boost::add_edge(current_vertex, dep_vertex, dep_graph);

            // 6. If the dependency is a local file, add it to the queue to be processed
            if (std::filesystem::exists(script_dir + "/" + dep_name + ".py")) {
                dep_graph[dep_vertex].type = "File";
                dep_graph[dep_vertex].color = "skyblue";
                files_to_process.push(dep_name); // This creates the multi-layer effect
            } else {
                dep_graph[dep_vertex].type = "Library";
                dep_graph[dep_vertex].color = "palegreen";
            }
        }
    }

    // 7. Write the final, complete graph to a .dot file
    std::ofstream dot_file("output/dependency_graph.dot");
    boost::write_graphviz(dot_file, dep_graph,
        [&](std::ostream& out, const DependencyVertex& v) {
            out << "[label=\"" << dep_graph[v].name <<
                // << "\", style=filled, fillcolor=" << dep_graph[v].color <<
                 "\"]";
        }
    );
    std::cout << "Multi-layer dependency graph written to output/dependency_graph.dot" << std::endl;
}

void write_instructions_to_file(
    const std::map<std::string, std::vector<Instruction>>& instruction_map,
    const std::string& output_dir_path
) {
    if (!std::filesystem::exists(output_dir_path)) {
        if (!std::filesystem::create_directories(output_dir_path)) {
            std::cerr << "Error: Unable to create output directory: " << output_dir_path << "\n";
            return;
        }
    }

    for (const auto& pair_entry : instruction_map) {
        const std::string& func_name_str = pair_entry.first; 
        const std::vector<Instruction>& instructions_list = pair_entry.second; 

        std::string safe_name = func_name_str;
        std::replace(safe_name.begin(), safe_name.end(), '.', '_');
        std::replace(safe_name.begin(), safe_name.end(), '<', '_'); // For things like <lambda>
        std::replace(safe_name.begin(), safe_name.end(), '>', '_');
        std::string filename = output_dir_path + "/" + safe_name + ".txt";

        std::ofstream file_stream(filename); 
        if (!file_stream.is_open()) {
            std::cerr << "Error: Unable to write file for function: " << func_name_str << "\n";
            continue;
        }

        file_stream << "Bytecode Instructions for function: " << func_name_str << "\n";
        if (instructions_list.empty() && func_name_str.find("unresolved") == std::string::npos && func_name_str.find("C function") == std::string::npos) {
             file_stream << "(Likely a C function or built-in, or resolution failed)\n";
        }

        for (const auto& inst_detail : instructions_list) { 
            file_stream << inst_detail.opname << " (" << inst_detail.opcode << ")";
            if (inst_detail.arg != -1) {
                file_stream << " Arg: " << inst_detail.arg << " ArgVal: " << inst_detail.argval;
            }
            file_stream << "\n";
        }
        file_stream.close();
    }
}

void build_function_call_graph(Graph &graph, VertexMap &vertex_map,
                               const std::vector<Instruction> &instructions_list, 
                               const std::string &current_function_name, 
                               const std::map<std::string, std::vector<Instruction>>& all_known_functions) { // Pass all known to resolve calls
    Vertex current_vertex;
    if (vertex_map.find(current_function_name) == vertex_map.end()) {
        current_vertex = boost::add_vertex(graph);
        vertex_map[current_function_name] = current_vertex;
        graph[current_vertex].label = current_function_name;
    } else {
        current_vertex = vertex_map[current_function_name];
    }

    for (size_t i = 0; i < instructions_list.size(); ++i) {
        const auto &inst_detail = instructions_list[i]; 

        if (inst_detail.opname.rfind("CALL", 0) == 0) {
            std::string base_name, attr_name, called_func_id;
            find_callable_info_stack_based(instructions_list, i, base_name, attr_name, called_func_id);

            if (!called_func_id.empty()) {
                // Check if this called_func_id is one of the functions we successfully disassembled or marked
                if (all_known_functions.count(called_func_id)) {
                    Vertex called_vertex_desc; 
                    if (vertex_map.find(called_func_id) == vertex_map.end()) {
                        called_vertex_desc = boost::add_vertex(graph);
                        vertex_map[called_func_id] = called_vertex_desc;
                        graph[called_vertex_desc].label = called_func_id;
                    } else {
                        called_vertex_desc = vertex_map[called_func_id];
                    }

                    auto edge_pair = boost::edge(current_vertex, called_vertex_desc, graph);
                    if (edge_pair.second) { // Edge exists
                        graph[edge_pair.first].call_count++;
                    } else { // Edge doesn't exist
                        auto new_edge = boost::add_edge(current_vertex, called_vertex_desc, graph).first;
                        graph[new_edge].call_count = 1;
                    }
                } else {
                    // std::cout << "Graph: Did not find '" << called_func_id << "' in all_known_functions for edge from '" << current_function_name << "'." << std::endl;
                }
            } else if (&load_python_module && !inst_detail.argval.empty()) { // Fallback for simple CALL argval if find_callable_info fails
                 std::string direct_call_name = inst_detail.argval;
                 if (all_known_functions.count(direct_call_name)) {
                    Vertex called_vertex_desc; 
                    if (vertex_map.find(direct_call_name) == vertex_map.end()) {
                        called_vertex_desc = boost::add_vertex(graph);
                        vertex_map[direct_call_name] = called_vertex_desc;
                        graph[called_vertex_desc].label = direct_call_name;
                    } else {
                        called_vertex_desc = vertex_map[direct_call_name];
                    }
                    auto edge_pair = boost::edge(current_vertex, called_vertex_desc, graph);
                    if (edge_pair.second) { graph[edge_pair.first].call_count++; }
                    else { auto e = boost::add_edge(current_vertex, called_vertex_desc, graph).first; graph[e].call_count = 1; }
                 }
            }
        }
    }
}


std::map<std::string, std::shared_ptr<Graph>> write_function_call_graphs_to_dot(
    const std::string& output_dir_str, 
    const std::map<std::string, std::vector<Instruction>>& instruction_map_data 
) {
    namespace fs = std::filesystem;
    if (!fs::exists(output_dir_str)) {
        if (!fs::create_directories(output_dir_str)) {
            std::cerr << "Error: Unable to create output directory: " << output_dir_str << "\n";
            return {}; // Return empty map
        }
    }
    std::map<std::string, std::shared_ptr<Graph>> graph_map_data; 

    for (const auto& entry : instruction_map_data) {
        const std::string& func_name_key = entry.first; 
        const std::vector<Instruction>& instructions_val = entry.second; 

        if (instructions_val.empty() && instruction_map_data.at(func_name_key).empty()) { // Skip for C funcs / unresolved if they have no instructions
            // unless it's the main entry point perhaps? For now, skip if no instructions to draw from.
            // This check might be too aggressive if we want nodes for C functions.
            // The build_function_call_graph will add nodes if they are called, even if their own instruction list is empty.
        }

        auto subgraph_ptr = std::make_shared<Graph>(); 
        VertexMap sub_vertex_map_data; 

        // Build graph only for this function's direct calls based on its own instructions
        build_function_call_graph(*subgraph_ptr, sub_vertex_map_data, instructions_val, func_name_key, instruction_map_data);
        
        if (boost::num_vertices(*subgraph_ptr) > 0) { // Only write if graph is not empty
            graph_map_data[func_name_key] = subgraph_ptr;

            std::string safe_func_name = func_name_key;
            std::replace(safe_func_name.begin(), safe_func_name.end(), '.', '_');
            std::replace(safe_func_name.begin(), safe_func_name.end(), '<', '_');
            std::replace(safe_func_name.begin(), safe_func_name.end(), '>', '_');
            std::string filename_str = output_dir_str + "/" + safe_func_name + "_calls.dot"; 

            std::ofstream dot_file_stream(filename_str); 
            if (!dot_file_stream) {
                std::cerr << "Error: Could not write to file " << filename_str << "\n";
                continue;
            }

            boost::write_graphviz(
                dot_file_stream, *subgraph_ptr,
                boost::make_label_writer(boost::get(&VertexProperties::label, *subgraph_ptr)),
                boost::make_label_writer(boost::get(&EdgeProperties::call_count, *subgraph_ptr))
            );
            dot_file_stream.close();
        }
    }
    return graph_map_data;
}


Graph combine_all_function_graphs(
    const std::map<std::string, std::shared_ptr<Graph>>& graph_map_input, 
    const std::string& output_file_path, 
    const std::map<std::string, std::vector<Instruction>>& all_instructions // Used to ensure all known functions are nodes
) {
    Graph master_graph_obj; 
    std::unordered_map<std::string, Vertex> master_label_to_vertex;

    // First, ensure all known functions (even if not in a subgraph directly) have a vertex
    for (const auto& func_entry : all_instructions) {
        const std::string& label = func_entry.first;
        if (master_label_to_vertex.find(label) == master_label_to_vertex.end()) {
            Vertex new_v = boost::add_vertex(master_graph_obj);
            master_graph_obj[new_v].label = label;
            master_label_to_vertex[label] = new_v;
        }
    }


    for (const auto& pair_entry : graph_map_input) {
        const Graph& subgraph = *(pair_entry.second);
        
        // Temporary map from subgraph vertex descriptors to master_graph vertex descriptors
        std::unordered_map<Vertex, Vertex> temp_vertex_map; 

        // Copy vertices from subgraph to master_graph, handling duplicates by label
        for (auto vi = boost::vertices(subgraph).first; vi != boost::vertices(subgraph).second; ++vi) {
            const std::string& label = subgraph[*vi].label;
            Vertex master_v;
            if (master_label_to_vertex.find(label) == master_label_to_vertex.end()) {
                master_v = boost::add_vertex(master_graph_obj);
                master_graph_obj[master_v].label = label;
                master_label_to_vertex[label] = master_v;
            } else {
                master_v = master_label_to_vertex[label];
            }
            temp_vertex_map[*vi] = master_v; // Map subgraph vertex to its corresponding master graph vertex
        }

        // Copy edges from subgraph to master_graph
        for (auto ei = boost::edges(subgraph).first; ei != boost::edges(subgraph).second; ++ei) {
            Vertex sub_src_v = boost::source(*ei, subgraph);
            Vertex sub_tgt_v = boost::target(*ei, subgraph);
            EdgeProperties props = subgraph[*ei];

            Vertex master_src_v = temp_vertex_map[sub_src_v];
            Vertex master_tgt_v = temp_vertex_map[sub_tgt_v];

            // Add edge to master_graph and aggregate call_count
            auto edge_pair_master = boost::edge(master_src_v, master_tgt_v, master_graph_obj);
            if (edge_pair_master.second) { // Edge already exists in master
                master_graph_obj[edge_pair_master.first].call_count += props.call_count;
            } else { // Edge doesn't exist, add new one
                auto new_master_edge = boost::add_edge(master_src_v, master_tgt_v, master_graph_obj).first;
                master_graph_obj[new_master_edge].call_count = props.call_count;
            }
        }
    }

    std::ofstream dot_file_stream(output_file_path); 
    if (!dot_file_stream) {
        std::cerr << "Error: could not open file for writing: " << output_file_path << "\n";
    } else {
        boost::write_graphviz(
            dot_file_stream, master_graph_obj,
            boost::make_label_writer(boost::get(&VertexProperties::label, master_graph_obj)),
            boost::make_label_writer(boost::get(&EdgeProperties::call_count, master_graph_obj))
        );
        dot_file_stream.close();
    }
    return master_graph_obj;
}


int main(int argc, char* argv[]) {
    if (argc < 2){ // Changed to < 2 because argv[0] is program name
        printf("Usage: %s <path_to_python_script_directory>\n", argv[0]);
        return 1;
    }

    std::string script_dir_path = argv[1];
    // Add the script's directory to Python's sys.path
    // Ensure path is properly escaped if it contains spaces or special chars (basic handling here)
    std::string python_path_command = "import sys\n";
    // Add current directory first
    python_path_command += "if '.' not in sys.path:\n"
                           "  sys.path.insert(0, '.')\n";
    // Add specified script directory
    std::string escaped_path = script_dir_path;
    std::replace(escaped_path.begin(), escaped_path.end(), '\\', '/'); // Normalize to forward slashes

    python_path_command += "script_path = r'" + escaped_path + "'\n"
                           "if script_path not in sys.path:\n"
                           "  sys.path.insert(0, script_path)\n";
    
    // std::cout << "Python path command:\n" << python_path_command << std::endl;


    Py_Initialize();
    if (PyRun_SimpleString(python_path_command.c_str()) != 0) {
        std::cerr << "Error setting Python path." << std::endl;
        PyErr_Print();
        Py_Finalize();
        return 1;
    }


    printf("Input Python script name (e.g., my_script, without .py): ");
    if (scanf("%255s", script_name) != 1) { Py_Finalize(); return 1;}

    PyObject *pModule = load_python_module(script_name);
    if (!pModule){
        Py_Finalize();
        return 1;
    }

    std::vector<std::string> classes_list; 
    std::vector<std::string> functions_list; 
    find_classes_and_functions(pModule, classes_list, functions_list);

    std::cout << "\nAvailable classes in " << script_name << ":\n";
    if (classes_list.empty()) std::cout << "  (No classes found)\n";
    for (const auto& cls_item : classes_list) std::cout << "  - " << cls_item << "\n"; 
    
    std::cout << "Available top-level functions in " << script_name << ":\n";
    if (functions_list.empty()) std::cout << "  (No functions found)\n";
    for (const auto& func_item : functions_list) std::cout << "  - " << func_item << "\n"; 

    std::string analysis_choice;
    std::cout << "\nAnalyze a [class] method or a top-level [function]? ";
    std::cin >> analysis_choice;

    PyObject* pFuncToAnalyze = nullptr; 
    std::string chosen_callable_name; // To store the "module.func" or "module.Class.method" style name


    if (analysis_choice == "function") {
        std::cout << "Enter function name to analyze: ";
        std::cin >> func_name; // Global func_name char array
        pFuncToAnalyze = load_python_function(pModule, func_name);
        if (pFuncToAnalyze) chosen_callable_name = std::string(script_name) + "." + func_name;
    } else if (analysis_choice == "class") {
        std::cout << "Enter class name: ";
        std::cin >> class_name; // Global class_name char array

        PyObject* pClassObj = PyObject_GetAttrString(pModule, class_name); 
        if (!pClassObj || !PyType_Check(pClassObj)) {
            PyErr_Print();
            std::cerr << "Invalid class name or object is not a class: " << class_name << "\n";
            Py_XDECREF(pClassObj);
        } else {
            std::vector<std::string> class_methods_list; 
            list_class_methods(pClassObj, class_methods_list);

            std::cout << "Callable methods in class " << class_name << ":\n";
            if (class_methods_list.empty()) std::cout << "  (No methods found or listed)\n";
            for (const auto& method_item : class_methods_list) 
                std::cout << "  - " << method_item << "\n";

            std::cout << "Enter method name to analyze (e.g., __init__ or regular method): ";
            std::cin >> func_name; // Global func_name used for method name

            // To get the actual function object for a method that can be disassembled,
            // it's often better to get it from an instance if possible, or ensure we get the underlying function.
            // For static analysis from class, PyObject_GetAttrString on class is usually fine.
            pFuncToAnalyze = PyObject_GetAttrString(pClassObj, func_name);
            if (!pFuncToAnalyze || !PyCallable_Check(pFuncToAnalyze)) {
                PyErr_Print();
                std::cerr << "Method " << func_name << " not found in class " << class_name << " or not callable.\n";
                Py_XDECREF(pFuncToAnalyze);
                pFuncToAnalyze = nullptr; // Ensure it's null
            } else {
                 chosen_callable_name = std::string(script_name) + "." + class_name + "." + func_name;
            }
            Py_DECREF(pClassObj);
        }
    } else {
        std::cerr << "Invalid choice.\n";
    }

    if (pFuncToAnalyze){
        std::cout << "\nStarting analysis for: " << chosen_callable_name << std::endl;
        // print_bytecode(pFuncToAnalyze); // Optional: print raw bytecode of initial function

        std::map<std::string, std::vector<Instruction>> instruction_map_data =
            disassemble_all_called_functions(pFuncToAnalyze, chosen_callable_name.c_str());
        
        std::filesystem::create_directories("output/bytecodes");
        write_instructions_to_file(instruction_map_data, "output/bytecodes");
        std::cout << "Bytecode instructions written to output/bytecodes/" << std::endl;

        std::filesystem::create_directories("output/graphs");
        std::map<std::string, std::shared_ptr<Graph>> graph_map_data =
            write_function_call_graphs_to_dot("output/graphs", instruction_map_data);
        std::cout << "Individual call graphs written to output/graphs/" << std::endl;
        
        if (!graph_map_data.empty() || !instruction_map_data.empty()){ // Ensure there's something to combine
             Graph master_graph_obj = combine_all_function_graphs(graph_map_data, "output/master_graph.dot", instruction_map_data);
             std::cout << "Master call graph written to output/master_graph.dot" << std::endl;
        } else {
            std::cout << "No data generated for master graph." << std::endl;
        }

            std::cout << "\nGenerating Control Flow Graphs..." << std::endl;

        std::filesystem::create_directories("output/cfgs");

        for (const auto& entry : instruction_map_data) {
            const std::string& func_name = entry.first;
            const std::vector<Instruction>& instructions = entry.second;

            if (instructions.empty()) {
                std::cout << "Skipping CFG for " << func_name << " (no instructions)." << std::endl;
                continue;
            }

            ControlFlowGraph cfg = generate_cfg_from_instructions(instructions);
            
            std::string safe_name = func_name;
            std::replace(safe_name.begin(), safe_name.end(), '.', '_');
            std::string cfg_filename = "output/cfgs/" + safe_name + "_cfg.dot";
            
            write_cfg_to_dot(cfg_filename, cfg, func_name);
        }
        std::cout << "Control Flow Graphs written to output/cfgs/" << std::endl;

        generate_dependency_graph(script_name, script_dir_path);

        Py_DECREF(pFuncToAnalyze);
    } else {
         std::cout << "No function or method selected for analysis, or failed to load." << std::endl;
    }

    Py_DECREF(pModule);
    Py_Finalize();
    return 0;
}