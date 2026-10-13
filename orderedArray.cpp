#include <iostream>
#include <vector>
using namespace std;

class OrdArray {
private:
    vector<double> v;
    int nElems;

public:
    OrdArray(int max) : nElems(0) {
        v.resize(max);
    }

    int getSize() {
        return nElems;
    }

    // binary search: returns the index of searchKey,
    // or nElems if searchKey isn't in the array
    int find(double searchKey) {
        int lowerBound = 0;
        int upperBound = nElems - 1;

        // keep going while the range still holds at least one cell
        while (lowerBound <= upperBound) {
            // look at the cell in the middle of the range
            int curIn = (lowerBound + upperBound) / 2;
            if (v[curIn] == searchKey)
                return curIn;

            // throw away the half that can't contain the key
            if (v[curIn] < searchKey)
                lowerBound = curIn + 1;
            else
                upperBound = curIn - 1;
        }
        // the range is empty, so the key is not here
        return nElems;
    }
};