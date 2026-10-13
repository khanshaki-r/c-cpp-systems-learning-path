#include <iostream>
#include <vector> 
using namespace std;

class LowArray {
    private:
        vector<double> v;

    public:
        LowArray(int max){
            v.resize(max);
        }

        void setElem(int index, int value){
            v[index] = value;
        }

        double getElem(int index){
           return v[index];
        }
};
int main() {
    LowArray arr(100);
    int nElems = 0;
    int j;

    // main decides where each item goes
    double arrivals[] = {77, 99, 44, 55, 22, 88, 11, 0, 66, 33};
    for (j = 0; j < 10; j++) {
        arr.setElem(nElems, arrivals[j]);
        nElems++;
    }
    for (j = 0; j < nElems; j++)
        cout << arr.getElem(j) << " ";
    cout << endl;

    // main runs the linear search itself
    double searchKey = 26;
    for (j = 0; j < nElems; j++)
        if (arr.getElem(j) == searchKey)
            break;
    if (j == nElems)
        cout << "Can't find " << searchKey << endl;
    else
        cout << "Found " << searchKey << endl;

    // main shifts the items after a deletion, one cell at a time
    searchKey = 55;
    for (j = 0; j < nElems; j++)
        if (arr.getElem(j) == searchKey)
            break;
    if (j < nElems) {
        for (int k = j; k < nElems - 1; k++)
            arr.setElem(k, arr.getElem(k + 1));
        nElems--;
    }
    for (j = 0; j < nElems; j++)
        cout << arr.getElem(j) << " ";
    cout << endl;
    return 0;
}
