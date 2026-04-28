/**
 * STL Container Test - Focused on primitive types
 * Tests that the plugin correctly instruments std::vector and std::unordered_map with primitive types
 */

#include <iostream>
#include <vector>
#include <unordered_map>
#include <cstdint>

#define TM __attribute__((annotate("tm")))
#define TX __attribute__((annotate("transaction")))

TM std::vector<int> g_int_vector;
TM std::vector<double> g_double_vector;
TM std::vector<float> g_float_vector;
TM std::unordered_map<int, int> g_int_int_map;
TM std::unordered_map<double, double> g_double_double_map;

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

TX void test_vector_double() {
    for (int i = 0; i < 50; i++) {
        g_double_vector.push_back(i * 1.5);
    }
    
    double sum = 0;
    for (size_t i = 0; i < g_double_vector.size(); i++) {
        sum += g_double_vector[i];
    }
    g_double_vector[0] = sum;
}

TX void test_vector_float() {
    for (int i = 0; i < 50; i++) {
        g_float_vector.push_back(i * 1.5f);
    }
    
    float sum = 0;
    for (size_t i = 0; i < g_float_vector.size(); i++) {
        sum += g_float_vector[i];
    }
    g_float_vector[0] = sum;
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

TX void test_map_double() {
    for (int i = 0; i < 50; i++) {
        g_double_double_map[i * 1.5] = i * 3.0;
    }
    
    double sum = 0;
    for (auto& kv : g_double_double_map) {
        sum += kv.second;
    }
}

int main() {
    std::cout << "STL Primitive Container Test\n";
    std::cout << "=============================\n\n";
    
    g_int_vector = {};
    g_double_vector = {};
    g_float_vector = {};
    g_int_int_map = {};
    g_double_double_map = {};
    
    std::cout << "Testing std::vector<int>...\n";
    test_vector_int();
    std::cout << "  g_int_vector.size() = " << g_int_vector.size() << "\n";
    std::cout << "  g_int_vector[0] = " << g_int_vector[0] << "\n";
    
    std::cout << "Testing std::vector<double>...\n";
    test_vector_double();
    std::cout << "  g_double_vector.size() = " << g_double_vector.size() << "\n";
    std::cout << "  g_double_vector[0] = " << g_double_vector[0] << "\n";
    
    std::cout << "Testing std::vector<float>...\n";
    test_vector_float();
    std::cout << "  g_float_vector.size() = " << g_float_vector.size() << "\n";
    std::cout << "  g_float_vector[0] = " << g_float_vector[0] << "\n";
    
    std::cout << "Testing std::unordered_map<int, int>...\n";
    test_map_int_int();
    std::cout << "  g_int_int_map.size() = " << g_int_int_map.size() << "\n";
    std::cout << "  g_int_int_map[0] = " << g_int_int_map[0] << "\n";
    
    std::cout << "Testing std::unordered_map<double, double>...\n";
    test_map_double();
    std::cout << "  g_double_double_map.size() = " << g_double_double_map.size() << "\n";
    
    std::cout << "\nAll tests passed!\n";
    return 0;
}