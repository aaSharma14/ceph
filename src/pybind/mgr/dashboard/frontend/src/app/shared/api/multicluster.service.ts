import { HttpClient, HttpParams } from '@angular/common/http';
import { Injectable } from '@angular/core';
import { BehaviorSubject } from 'rxjs';

@Injectable({
  providedIn: 'root'
})
export class MulticlusterService {
  
  private selectionChangedSource = new BehaviorSubject<string>('');

  selectionChanged$ = this.selectionChangedSource.asObservable();

  emitSelectionChanged(value: string): void {
    this.selectionChangedSource.next(value);
  }

  getRemoteData(remotedata: any) {
    const url = remotedata['remoteClusterUrl'];
    const token = remotedata['apiToken'];
    let params = new HttpParams();
    params = params.appendAll({
      remote_cluster_url: url,
      apiToken: token
    })
    console.log(params);
    
    return this.http.get(`api/multicluster/route`, { params: params });
  }

  constructor(private http: HttpClient) {}

  addRemoteCluster(apiToken: string, remoteClusterUrl: string) {
    let requestBody = {
      apiToken: apiToken,
      remoteClusterUrl: remoteClusterUrl
    }
    return this.http.post(`api/health/add_remote_cluster`, requestBody);
  }

  getRemoteClusterUrls() {
    return this.http.get(`api/health/get_remote_cluster_urls`);
  }

  
}
