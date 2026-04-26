#include <iostream>

struct Node {
    int data;
    Node* left;
    Node* right;

    Node(int value) {
        data = value;
        left = nullptr;
        right = nullptr;
    }
};

class BST {
private:
    Node* root;

    Node* insertHelper(Node* node, int value) {
        if (node == nullptr) {
            return new Node(value);
        }

        if (value < node->data) {
            node->left = insertHelper(node->left, value);
        } else if (value > node->data) {
            node->right = insertHelper(node->right, value);
        }

        return node;
    }

    bool searchHelper(Node* node, int value) {
        if (node == nullptr) {
            return false;
        }
        if (node->data == value) {
            return true;
        }

        if (node->data < value) {
            return searchHelper(node->right, value);
        }

        return searchHelper(node->left, value);
    }

    void inorderHelper(Node* node) {
        if (node != nullptr) {
            inorderHelper(node->left);
            std::cout << node->data << " ";
            inorderHelper(node->right);
        }
    }

    void destroyTree(Node* node) {
        if (node != nullptr) {
            destroyTree(node->left);
            destroyTree(node->right);
            delete node;
        }
    }

public:
    BST() {
        root = nullptr;
    }

    ~BST() {
        destroyTree(root);
    }

    void insert(int value) {
        root = insertHelper(root, value);
    }

    bool search(int value) {
        return searchHelper(root, value);
    }

    void inorder() {
        inorderHelper(root);
        std::cout << "\n";
    }
};

int main() {
    BST tree;

    tree.insert(50);
    tree.insert(30);
    tree.insert(20);
    tree.insert(40);
    tree.insert(70);
    tree.insert(60);
    tree.insert(80);

    tree.inorder();

    std::cout << "40: " << (tree.search(40) ? "yes" : "no") << "\n";
    std::cout << "90: " << (tree.search(90) ? "yes" : "no") << "\n";

    return 0;
}
