let RED = 1;
let BLACK = 0;

let root = -1;

let nodeKey = [];
let nodeColor = [];
let nodeLeft = [];
let nodeRight = [];
let nodeParent = [];

let freeStack = [];
let freeTop = 0;
let nodeCount = 0;

function createNode(key) {
    let i;

    if (freeTop > 0) {
        freeTop = freeTop - 1;
        i = freeStack[freeTop];
    } else {
        i = nodeCount;
        nodeCount = nodeCount + 1;
    }

    nodeKey[i] = key;
    nodeColor[i] = RED;
    nodeLeft[i] = -1;
    nodeRight[i] = -1;
    nodeParent[i] = -1;

    return i;
}

function colorOf(i) {
    if (i == -1) {
        return BLACK;
    }
    return nodeColor[i];
}

function leftRotate(x) {
    let y;
    y = nodeRight[x];

    nodeRight[x] = nodeLeft[y];
    if (nodeLeft[y] != -1) {
        nodeParent[nodeLeft[y]] = x;
    }

    nodeParent[y] = nodeParent[x];

    if (nodeParent[x] == -1) {
        root = y;
    } else if (x == nodeLeft[nodeParent[x]]) {
        nodeLeft[nodeParent[x]] = y;
    } else {
        nodeRight[nodeParent[x]] = y;
    }

    nodeLeft[y] = x;
    nodeParent[x] = y;
}

function rightRotate(x) {
    let y;
    y = nodeLeft[x];

    nodeLeft[x] = nodeRight[y];
    if (nodeRight[y] != -1) {
        nodeParent[nodeRight[y]] = x;
    }

    nodeParent[y] = nodeParent[x];

    if (nodeParent[x] == -1) {
        root = y;
    } else if (x == nodeRight[nodeParent[x]]) {
        nodeRight[nodeParent[x]] = y;
    } else {
        nodeLeft[nodeParent[x]] = y;
    }

    nodeRight[y] = x;
    nodeParent[x] = y;
}

function insertFixup(z) {
    let p;
    let g;
    let y;

    while (z != root && colorOf(nodeParent[z]) == RED) {
        p = nodeParent[z];
        g = nodeParent[p];

        if (p == nodeLeft[g]) {
            y = nodeRight[g];

            if (y != -1 && nodeColor[y] == RED) {
                nodeColor[p] = BLACK;
                nodeColor[y] = BLACK;
                nodeColor[g] = RED;
                z = g;
            } else {
                if (z == nodeRight[p]) {
                    z = p;
                    leftRotate(z);
                    p = nodeParent[z];
                    g = nodeParent[p];
                }

                nodeColor[p] = BLACK;
                nodeColor[g] = RED;
                rightRotate(g);
            }
        } else {
            y = nodeLeft[g];

            if (y != -1 && nodeColor[y] == RED) {
                nodeColor[p] = BLACK;
                nodeColor[y] = BLACK;
                nodeColor[g] = RED;
                z = g;
            } else {
                if (z == nodeLeft[p]) {
                    z = p;
                    rightRotate(z);
                    p = nodeParent[z];
                    g = nodeParent[p];
                }

                nodeColor[p] = BLACK;
                nodeColor[g] = RED;
                leftRotate(g);
            }
        }
    }

    nodeColor[root] = BLACK;
}

function insert(key) {
    let z;
    let x;
    let y;

    z = createNode(key);
    y = -1;
    x = root;

    while (x != -1) {
        y = x;
        if (key < nodeKey[x]) {
            x = nodeLeft[x];
        } else {
            x = nodeRight[x];
        }
    }

    nodeParent[z] = y;

    if (y == -1) {
        root = z;
    } else if (key < nodeKey[y]) {
        nodeLeft[y] = z;
    } else {
        nodeRight[y] = z;
    }

    insertFixup(z);
}

function search(key) {
    let x;

    x = root;
    while (x != -1) {
        if (key == nodeKey[x]) {
            return x;
        }
        if (key < nodeKey[x]) {
            x = nodeLeft[x];
        } else {
            x = nodeRight[x];
        }
    }

    return -1;
}

function inorder(node, visit) {
    if (node == -1) {
        return;
    }

    inorder(nodeLeft[node], visit);
    visit(nodeKey[node]);
    inorder(nodeRight[node], visit);
}

function printValue(v) {
    console.log(v);
}

insert(10);
insert(20);
insert(30);
insert(15);
insert(25);
insert(5);
// insert(6);

inorder(root, printValue);

let p;
p = search(15);
console.log(p);