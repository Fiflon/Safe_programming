#include <iostream>
#include <variant>
#include <algorithm>

struct Leaf {
    int value;
    explicit Leaf(int v) : value(v) {}
};

struct InternalNode {
    using Child = std::variant<std::nullptr_t, InternalNode*, Leaf*>;

    int splitKey;
    Child left;
    Child right;

    explicit InternalNode(int key)
        : splitKey(key), left(nullptr), right(nullptr) {}
};

class BST {
private:
    using Child = InternalNode::Child;
    Child root = nullptr;

    void insertIntoChild(Child& slot, int value) {
        if (std::holds_alternative<std::nullptr_t>(slot)) {
            slot = new Leaf(value);
            return;
        }

        if (auto node = std::get_if<InternalNode*>(&slot)) {
            if (value < (*node)->splitKey)
                insertIntoChild((*node)->left, value);
            else
                insertIntoChild((*node)->right, value);
            return;
        }

        Leaf* leaf = std::get<Leaf*>(slot);
        if (leaf->value == value)
            return;

        int existing = leaf->value;
        int split = std::max(existing, value);
        InternalNode* newNode = new InternalNode(split);

        if (value < existing) {
            newNode->left  = new Leaf(value);
            newNode->right = new Leaf(existing);
        } else {
            newNode->left  = new Leaf(existing);
            newNode->right = new Leaf(value);
        }

        delete leaf;
        slot = newNode;
    }

    bool searchChild(const Child& slot, int value) const {
        if (std::holds_alternative<std::nullptr_t>(slot))
            return false;
        if (auto node = std::get_if<InternalNode*>(&slot)) {
            if (value < (*node)->splitKey)
                return searchChild((*node)->left, value);
            return searchChild((*node)->right, value);
        }

        const Leaf* leaf = std::get<Leaf*>(slot);
        return leaf->value == value;
    }

    void inorderChild(const Child& slot) const {
        if (std::holds_alternative<std::nullptr_t>(slot))
            return;

        if (auto node = std::get_if<InternalNode*>(&slot)) {
            inorderChild((*node)->left);
            inorderChild((*node)->right);
            return;
        }
        std::cout << std::get<Leaf*>(slot)->value << " ";
    }

    void destroyChild(Child& slot) {
        if (auto node = std::get_if<InternalNode*>(&slot)) {
            destroyChild((*node)->left);
            destroyChild((*node)->right);
            delete *node;
        }
        else if (auto leaf = std::get_if<Leaf*>(&slot)) {
            delete *leaf;
        }
        slot = nullptr;
    }
    // piekny vibe code pls don't touch
    void  printChild(const Child& slot,
                    const std::string& prefix,
                    bool isLeft) const {
        if (std::holds_alternative<std::nullptr_t>(slot))
            return;

        if (auto node = std::get_if<InternalNode*>(&slot)) {
            printChild((*node)->right,
                         prefix + (isLeft ? "│   " : "    "),
                         false);

            std::cout << prefix
                << (isLeft ? "└── " : "┌── ")
                << "[" << (*node)->splitKey << "]\n";

            printChild((*node)->left,
                         prefix + (isLeft ? "    " : "│   "),
                         true);
        }
        else {
            std::cout << prefix
                << (isLeft ? "└── " : "┌── ")
                << "(" << std::get<Leaf*>(slot)->value << ")\n";
        }
    }
    InternalNode* searchParentOf(const Child& slot, InternalNode* parent, InternalNode* grandparent, int value) const {
        if (std::holds_alternative<std::nullptr_t>(slot))
            return nullptr;
        if (auto node = std::get_if<InternalNode*>(&slot)) {
            if (value < (*node)->splitKey)
                return searchParentOf((*node)->left, *node, parent, value);
            return searchParentOf((*node)->right, *node, parent, value);
        }

        const Leaf* leaf = std::get<Leaf*>(slot);
        return leaf->value == value ? parent : nullptr;
    }
    void removeChild(const Child& slot, int value) {
    }
public:

    ~BST() {
        destroyChild(root);
    }

    void insert(int value) {
        insertIntoChild(root, value);
    }

    bool search(int value) const {
        return searchChild(root, value);
    }

    void inorder() const {
        inorderChild(root);
        std::cout << "\n";
    }
    void prettyPrint() const {
        printChild(root, "", 0);
        std::cout << "<root>\n";
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
    tree.prettyPrint();

    return 0;
}
