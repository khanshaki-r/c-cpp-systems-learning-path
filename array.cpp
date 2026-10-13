#include <iostream>
using namespace std;

int main()
{
    int arrivals[] = {1,3,2,4,5,8,6,7,9};
    int length = sizeof(arrivals)/sizeof(arrivals[0]);
    int arr[100];
    int nElements = 0;
    int j = 0;

    //insertion

    for(int j = 0; j<length; j++)
    {
        arr[j] = arrivals[j];
    }

    //display
    for(j = 0; j<length; j++)
    {
        cout << arr[j] << " ";
        cout << endl;
    }

    //Liner search for 7

    int searchKey = 7;
    for(j = 0; j<length; j++)
    {
        if(arr[j] == searchKey)
        {
            break;
        }
    }
    if(j == length)
    {
        cout << "Can't find " << searchKey << endl;
    }else
    {
        cout << "Found " << searchKey << endl;
    }

    //Deletion

    searchKey = 9;

    for(j = 0 ; j< length; j++)
    {
        if(arr[j] == searchKey)
        {
            break;
        }
    }
    for(int k =j; k<length; k++)
    {
        arr[k] = arr[k+1];
    }

}