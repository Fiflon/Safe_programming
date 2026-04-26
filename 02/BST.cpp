#include <iostream>
#include <variant>

struct Leaf {
    int value;

    explicit Leaf(int inputValue) : value(inputValue) {}
};

enum class eStatus {
    Idle,
    Flagged,
    Marked
};

struct ComperableNode {
    using Child = std::variant<std::nullptr_t, ComperableNode*, Leaf*>;

    eStatus status;
    int value;
    Child left;
    Child right;

    explicit ComperableNode(int inputValue) : value(inputValue), left(nullptr), right(nullptr), status(eStatus::Idle) {}
};


class BST {
private:
    ComperableNode* root;

    bool isNull(const ComperableNode::Child& childSlot) const {
        return std::holds_alternative<std::nullptr_t>(childSlot);
    }

    bool isLeaf(const ComperableNode::Child& childSlot) const {
        return std::holds_alternative<Leaf*>(childSlot);
    }

    bool isNode(const ComperableNode::Child& childSlot) const {
        return std::holds_alternative<ComperableNode*>(childSlot);
    }
    void insertIntoChild(ComperableNode::Child& childSlot, int value) {
        if (isNull(childSlot)) {
            childSlot = new Leaf(value);
            return;
        }

        if (auto nextNode = std::get_if<ComperableNode*>(&childSlot)) {
            insertHelper(*nextNode, value);
            return;
        }

        Leaf* leaf = std::get<Leaf*>(childSlot);
        if (leaf->value == value) {
            return;
        }

        ComperableNode* promotedNode = new ComperableNode(leaf->value);
        if (value < promotedNode->value) {
            promotedNode->left = new Leaf(value);
        } else {
            promotedNode->right = new Leaf(value);
        }

        delete leaf;
        childSlot = promotedNode;
    }

    void insertHelper(ComperableNode* node, int value) {
        if (value < node->value) {
            insertIntoChild(node->left, value);
        } else if (value > node->value) {
            insertIntoChild(node->right, value);
        }
    }

    bool searchInChild(const ComperableNode::Child& childSlot, int value) const {
        if (isNull(childSlot)) {
            return false;
        }

        if (const auto nextNode = std::get_if<ComperableNode*>(&childSlot)) {
            return searchHelper(*nextNode, value);
        }

        const Leaf* leaf = std::get<Leaf*>(childSlot);
        return leaf->value == value;
    }

    bool searchHelper(const ComperableNode* node, int value) const {
        if (node == nullptr) {
            return false;
        }
        if (node->value == value) {
            return true;
        }

        if (value < node->value) {
            return searchInChild(node->left, value);
        }

        return searchInChild(node->right, value);
    }

    void inOrderChild(const ComperableNode::Child& childSlot) const {
        if (isNull(childSlot)) {
            return;
        }

        if (const auto nextNode = std::get_if<ComperableNode*>(&childSlot)) {
            inOrderHelper(*nextNode);
            return;
        }

        std::cout << std::get<Leaf*>(childSlot)->value << " ";
    }

    void inOrderHelper(const ComperableNode* node) const {
        if (node != nullptr) {
            inOrderChild(node->left);
            std::cout << node->value << " ";
            inOrderChild(node->right);
        }
    }

    void destroyChild(ComperableNode::Child& childSlot) {
        if (auto nextNode = std::get_if<ComperableNode*>(&childSlot)) {
            destroyTree(*nextNode);
        } else if (auto leaf = std::get_if<Leaf*>(&childSlot)) {
            delete *leaf;
        }

        childSlot = nullptr;
    }

    void destroyTree(ComperableNode* node) {
        if (node != nullptr) {
            destroyChild(node->left);
            destroyChild(node->right);
            delete node;
        }
    }

public:
    BST() : root(nullptr) {}

    ~BST() {
        destroyTree(root);
    }

    void insert(int value) {
        if (root == nullptr) {
            root = new ComperableNode(value);
            return;
        }

        insertHelper(root, value);
    }

    bool search(int value) const {
        return searchHelper(root, value);
    }

    void inorder() const {
        inOrderHelper(root);
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
