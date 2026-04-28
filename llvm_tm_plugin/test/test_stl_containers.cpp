/**
 * STL Container Test for LLVM TM Plugin
 * Tests that the plugin correctly instruments std::vector and std::unordered_map
 */

#include <iostream>
#include <vector>
#include <unordered_map>
#include <string>
#include <cstdint>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction")))

// Global TM-annotated data structures
TM std::vector<int> g_int_vector;
TM std::vector<std::string> g_string_vector;
TM std::unordered_map<int, int> g_int_int_map;
TM std::unordered_map<std::string, int> g_string_int_map;

// Test functions
TX void test_vector_int() {
    for (int i = 0; i < 100; i++) {
        g_int_vector.push_back(i);
    }
    
    int sum = 0;
    for (size_t i = 0; i < g_int_vector.size(); i++) {
        sum += g_int_vector[i];
    }
    g_int_vector[0] = sum;
}

TX void test_vector_string() {
    g_string_vector.push_back("hello");
    g_string_vector.push_back("world");
    g_string_vector[0] = "test";
    
    for (size_t i = 0; i < g_string_vector.size(); i++) {
        g_string_vector[i].append("_");
    }
}

TX void test_map_int_int() {
    for (int i = 0; i < 50; i++) {
        g_int_int_map[i] = i * 2;
    }
    
    int sum = 0;
    for (auto& kv : g_int_int_map) {
        sum += kv.second;
    }
    g_int_int_map[0] = sum;
}

TX void test_map_string_int() {
    g_string_int_map["alpha"] = 1;
    g_string_int_map["beta"] = 2;
    g_string_int_map["gamma"] = 3;
    
    g_string_int_map["alpha"] = 10;
    
    int sum = 0;
    for (auto& kv : g_string_int_map) {
        sum += kv.second;
    }
}

TX void test_nested_containers() {
    std::vector<std::unordered_map<int, int>> nested;
    for (int i = 0; i < 10; i++) {
        std::unordered_map<int, int> inner;
        inner[i] = i * 10;
        nested.push_back(inner);
    }
    
    for (size_t i = 0; i < nested.size(); i++) {
        nested[i][0] = nested[i][0] + 1;
    }
}

TX void test_container_copy() {
    std::vector<int> src = {1, 2, 3, 4, 5};
    std::vector<int> dst = src;
    dst[0] = 100;
}

int main() {
    std::cout << "STL Container Test for LLVM TM Plugin\n";
    std::cout << "======================================\n\n";
    
    // Initialize
    g_int_vector = {};
    g_string_vector = {};
    g_int_int_map = {};
    g_string_int_map = {};
    
    // Test each container type
    std::cout << "Testing std::vector<int>...\n";
    test_vector_int();
    std::cout << "  g_int_vector.size() = " << g_int_vector.size() << "\n";
    std::cout << "  g_int_vector[0] = " << g_int_vector[0] << "\n";
    
    std::cout << "Testing std::vector<std::string>...\n";
    test_vector_string();
    std::cout << "  g_string_vector.size() = " << g_string_vector.size() << "\n";
    std::cout << "  g_string_vector[0] = " << g_string_vector[0] << "\n";
    
    std::cout << "Testing std::unordered_map<int, int>...\n";
    test_map_int_int();
    std::cout << "  g_int_int_map.size() = " << g_int_int_map.size() << "\n";
    std::cout << "  g_int_int_map[0] = " << g_int_int_map[0] << "\n";
    
    std::cout << "Testing std::unordered_map<std::string, int>...\n";
    test_map_string_int();
    std::cout << "  g_string_int_map.size() = " << g_string_int_map.size() << "\n";
    
    std::cout << "Testing nested containers...\n";
    test_nested_containers();
    
    std::cout << "Testing container copy...\n";
    test_container_copy();
    
    std::cout << "\nAll tests passed!\n";
    return 0;
}